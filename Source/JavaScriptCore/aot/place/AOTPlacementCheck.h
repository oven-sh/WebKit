/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTPlacement.h"
#include "AOTSort.h"
#include "JSExportMacros.h"
#include <algorithm>
#include <atomic>
#include <span>
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
    v(SiteCacheNotUsed)

enum class PlacementRule : uint32_t {
    None,
#define AOT_PLACEMENT_RULE(name) name,
    FOR_EACH_AOT_PLACEMENT_RULE(AOT_PLACEMENT_RULE)
#undef AOT_PLACEMENT_RULE
};

JS_EXPORT_PRIVATE ASCIILiteral nameOf(PlacementRule);

inline constexpr unsigned maxReportedPlacementFailures = 100;

class PlacementFailureSink {
public:
    explicit PlacementFailureSink(BuildContext& build)
        : m_build(build)
    {
    }

    void fail(PlacementRule rule, LayoutID layout = { }, FunctionID function = { }, ValueID value = { })
    {
        if (m_failures++ < maxReportedPlacementFailures)
            m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::PlacementCheckFailed, .function = function, .value = value, .layout = layout, .count = static_cast<uint32_t>(rule) });
    }

    bool hasFailed() const { return m_failures.load(std::memory_order_relaxed); }

private:
    BuildContext& m_build;
    std::atomic<unsigned> m_failures { 0 };
};

struct PlacementCheckContext {
    const Program& program;
    const PlacementInputs& inputs;
    const Placement& placement;
    const SiteCaches& siteCaches;
    BuildContext& build;
    PlacementFailureSink& failures;
};

struct Extent {
    uint64_t first { 0 };
    uint64_t end { 0 };
};

inline Extent extentAt(uint32_t offset, uint32_t size) { return { offset, static_cast<uint64_t>(offset) + size }; }

inline bool areDisjoint(std::span<Extent> extents, uint64_t start, uint64_t limit)
{
    AOT::sort(extents, { }, &Extent::first);
    uint64_t end = start;
    for (auto& extent : extents) {
        if (extent.first < end)
            return false;
        end = extent.end;
    }
    return end <= limit;
}

JS_EXPORT_PRIVATE void checkSlotPlan(const PlacementCheckContext&);
JS_EXPORT_PRIVATE void checkClassCreation(const PlacementCheckContext&);
JS_EXPORT_PRIVATE void checkClosureForms(const PlacementCheckContext&);
JS_EXPORT_PRIVATE void checkShapes(const PlacementCheckContext&);
JS_EXPORT_PRIVATE void checkFieldAccesses(const PlacementCheckContext&);
JS_EXPORT_PRIVATE void checkAllocations(const PlacementCheckContext&);
JS_EXPORT_PRIVATE void checkStatics(const PlacementCheckContext&);
JS_EXPORT_PRIVATE void checkGuardBits(const PlacementCheckContext&);
JS_EXPORT_PRIVATE void checkSites(const PlacementCheckContext&);
JS_EXPORT_PRIVATE void checkSiteCaches(const PlacementCheckContext&);

JS_EXPORT_PRIVATE bool canHold(const Program&, SlotRepresentation, const Type&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
