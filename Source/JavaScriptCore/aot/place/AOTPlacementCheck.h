/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

#define FOR_EACH_AOT_PLACEMENT_RULE(v) \
    v(MembersNotSorted) \
    v(EnvironmentHasMembers) \
    v(VariableLacksInitialValue) \
    v(LayoutStorageWrong) \
    v(SlotAlignment) \
    v(CellOverlap) \
    v(ExtensionOverlap) \
    v(SlotPastCellEnd) \
    v(RepresentationCannotHoldType) \
    v(AbsentStateMissing) \
    v(OutOfLineNotAllowed) \
    v(HiddenWordMissing) \
    v(WideReferenceMissing) \
    v(BytesKeptZeroNotFree) \
    v(DeferredClosureNotProven) \
    v(FieldRecords) \
    v(MemberCreationOrder) \
    v(LazyCreationNotAllowed) \
    v(ClosureFormNotProven) \
    v(ShapeNumbers) \
    v(ShapeVariantWrong) \
    v(MemberFailsTest) \
    v(TestPassesOthers) \
    v(TestFormNeedsKnownShapes) \
    v(MemberShapesWrong) \
    v(BitSetsOverlap) \
    v(AccessKey) \
    v(AccessLocationDiffers) \
    v(CoverageWrong) \
    v(ContentsTooNarrow) \
    v(AccessTestWrong) \
    v(AllocationOfUnplacedLayout) \
    v(AllocationShape) \
    v(AllocationSize) \
    v(InitialFieldWithoutStorage) \
    v(AllocationOrderWord) \
    v(KeyOrderInvalid) \
    v(VariableOverlap) \
    v(VariableRegion) \
    v(VariableWithoutStorage) \
    v(EncodingNotAllowed) \
    v(RootMissing) \
    v(ClearedRangeIsFilledAtProgramEntry) \
    v(GuardBitsNotDistinct) \
    v(GuardWordOverlap) \
    v(PayloadMissing) \
    v(SiteRefersElsewhere) \
    v(StoredTypeTooWide) \
    v(DeferredClosureStore) \
    v(RegExpRecordWrong) \
    v(RequestWrong) \
    v(PropertyRunWrong) \
    v(DataNotPlaced) \
    v(SiteCacheNotDistinct) \
    v(SiteCacheOrder) \
    v(SiteCacheNotUsed) \
    v(ValueInAccessorSlot)

enum class PlacementRule : uint32_t {
    None,
#define AOT_PLACEMENT_RULE(name) name,
    FOR_EACH_AOT_PLACEMENT_RULE(AOT_PLACEMENT_RULE)
#undef AOT_PLACEMENT_RULE
};

JS_EXPORT_PRIVATE ASCIILiteral nameOf(PlacementRule);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
