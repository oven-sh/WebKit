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
void willWorkOnThisThread(VM&);
void didChangeBarrierThreshold(VM&, unsigned threshold, bool mutatorShouldBeFenced);
void willAnalyzeHeap(VM&);
void willCollectForExhaustedCage(VM&);
void didFindNoRoomWhereCollectionMayRun(VM&);
String compiledCalleeName(JSCell*);
bool representsModuleInHeapSnapshot(JSCell*);
[[noreturn]] void crashOnCellVisitMismatch(VM&, const JSCell*);
std::span<const uint32_t> narrowReferenceOffsets(const Structure*);

} // namespace HeapHooks

JS_EXPORT_PRIVATE WTF::Vector<std::unique_ptr<MarkingConstraint>> createMarkingConstraints(VM&);
JS_EXPORT_PRIVATE void initializeCollectorWords(VM&, Instance&);
JS_EXPORT_PRIVATE void setBarrierThreshold(Instance&, unsigned threshold);

class MutatorMarkStackLayout final : public MarkStackArray {
public:
    using Segment = GCArraySegment<const JSCell*>;

    static constexpr size_t segmentCapacity = CapacityFromSize<Segment::blockSize>::value;
    static constexpr ptrdiff_t offsetOfFirstCellInSegment = sizeof(Segment);
#if ASSERT_ENABLED
    static constexpr ptrdiff_t offsetOfTopInSegment = OBJECT_OFFSETOF(Segment, m_top);
#endif
    static constexpr ptrdiff_t offsetOfTop() { return OBJECT_OFFSETOF(MutatorMarkStackLayout, m_top); }
    static constexpr ptrdiff_t offsetOfHeadSegment() { return OBJECT_OFFSETOF(MutatorMarkStackLayout, m_segments); }
    static Segment* headSegmentOf(MarkStackArray& stack) { return static_cast<MutatorMarkStackLayout&>(stack).m_segments.head(); }
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
