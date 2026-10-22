/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CollectionScope.h"
#include "JSExportMacros.h"
#include <wtf/Forward.h>

namespace JSC {

class JSCell;
class VM;

namespace AOT {

struct Instance;

namespace HeapHooks {

// What --aot-sabotage left out of this executable (Wire::Sabotage). Zero in every product. Asked only at places that are rare.
extern JS_EXPORT_PRIVATE uint8_t sabotage;
enum class SabotagedPart : uint8_t {
    StackNamesNoInnerCell = 1,
    NarrowReferenceOnStackNamesNoInnerCell,
    InnerObjectIsVisitedWithoutItsFields,
    InnerArrayIsVisitedWithoutItsElements,
    InnerCellVisitedInPlaceStaysWhite,
    InnerCellIsMarkedWithoutItsOuterCell,
    InnerCellLetGoKeepsItsHeader,
};
ALWAYS_INLINE bool isSabotaged(SabotagedPart part) { return sabotage == static_cast<uint8_t>(part); }

void finalize(VM&, CollectionScope);
void didChangeBarrierThreshold(VM&, unsigned threshold, bool mutatorShouldBeFenced);
void willAnalyzeHeap(VM&);
String nameOfCompiledCallee(JSCell*);
[[noreturn]] void crashOnCellVisitMismatch(VM&, const JSCell*);
// The inner cell of a live outer cell that the address lies in, storage behind the inner cell included. Null in front of the first one, and for room that nothing has filled yet.
JSCell* innerCellAround(JSCell* outerCell, const void* address);
// For a thread that marks, behind the visit of an outer cell: the inner cells in it that have a header and no mark are looked at again when the marking ends.
void noteInnerCellsWithoutMark(JSCell* outerCell);
// With the mutators stopped, before anything is swept. An inner cell that was noted and is still without a mark is referred to by nothing: it loses its header, so that no later scan of a stack takes it for a cell.
void didFinishMarking(VM&);
// For a validating image, while the mutators are stopped. Allocates nothing and runs no code, so a thread that marks may call it.
void validateSlotsOf(VM&, const JSCell* compiledObject);

} // namespace HeapHooks

JS_EXPORT_PRIVATE void addMarkingConstraints(VM&);
JS_EXPORT_PRIVATE void initializeCollectorWords(VM&, Instance&);

// For a validating image. A function cell in a frame is only ever called: its properties would lie on the heap, outlive the frame and point back into it.
void validateFunctionCellIsNotInFrame(VM&, const JSCell*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
