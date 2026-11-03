/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCellSizeClasses.h"
#include "AOTSiteWeight.h"
#include "AOTSlotPlan.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <compare>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastBitVector.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

using SlotRow = uint32_t;

inline constexpr uint32_t noSharedLocation = UINT32_MAX;
inline constexpr uint32_t referencesBeyondCreation = 4;
inline constexpr uint32_t maxInlineSize = 128;
AOT_THRESHOLD(uint32_t, maxInlineSizeAtCreation, 512);
static_assert(offsetOfFirstField + maxFieldsAlwaysInline * widthOf(SlotRepresentation::JSValue) == maxInlineSize);

class SlotRows {
public:
    SlotRows() = default;
    JS_EXPORT_PRIVATE explicit SlotRows(const Program&);

    uint32_t count() const { return m_firstRow.last(); }
    SlotRow firstRowOf(LayoutID id) const { return m_firstRow[id.index()]; }
    uint32_t numberOfRowsOf(LayoutID id) const { return m_firstRow[id.index() + 1] - m_firstRow[id.index()]; }
    uint32_t numberOfFieldsOf(LayoutID id) const { return numberOfRowsOf(id) - numberOfHiddenWords; }
    SlotRow rowOf(LayoutID id, FieldIndex field) const { return m_firstRow[id.index()] + field; }
    SlotRow extensionWordOf(LayoutID id) const { return m_firstRow[id.index() + 1] - numberOfHiddenWords; }
    SlotRow extraPropertiesWordOf(LayoutID id) const { return m_firstRow[id.index() + 1] - 1; }
    JS_EXPORT_PRIVATE LayoutID layoutOf(SlotRow) const;
    FieldIndex fieldOf(SlotRow row) const { return row - firstRowOf(layoutOf(row)); }
    bool isHiddenWord(SlotRow row) const { return row >= extensionWordOf(layoutOf(row)); }

private:
    Vector<uint32_t> m_firstRow;
};

class InheritedRows {
public:
    struct Family {
        SlotRow declaringRow { 0 };
        uint32_t firstRow { 0 };
        uint32_t numberOfRows { 0 };
        uint32_t firstRowInUnallocatedBases { 0 };
        uint32_t numberOfRowsInUnallocatedBases { 0 };
    };

    InheritedRows(const Program&, const SlotRows&, std::span<const LayoutStorage>);

    bool isInFamily(LayoutID id) const { return m_isInFamily.quickGet(id.index()); }
    uint32_t depthOf(LayoutID id) const { return m_depths[id.index()]; }
    SlotRow declaringRowOf(SlotRow row) const { return m_declaringRow[row]; }
    // By the depth of the declaring layout, then by declaring row.
    std::span<const Family> families() const LIFETIME_BOUND { return m_families.span(); }
    std::span<const SlotRow> rowsOf(const Family& family) const LIFETIME_BOUND { return m_rows.span().subspan(family.firstRow, family.numberOfRows); }
    std::span<const SlotRow> rowsInUnallocatedBasesOf(const Family& family) const LIFETIME_BOUND { return m_rowsInUnallocatedBases.span().subspan(family.firstRowInUnallocatedBases, family.numberOfRowsInUnallocatedBases); }
    // The compiled objects and their proven bases, a base before the layouts below it.
    std::span<const Vector<LayoutID>> layoutsByDepth() const LIFETIME_BOUND { return m_layoutsByDepth.span(); }

private:
    BitVector m_isInFamily;
    Vector<uint32_t> m_depths;
    Vector<SlotRow> m_declaringRow;
    Vector<Family> m_families;
    Vector<SlotRow> m_rows;
    Vector<SlotRow> m_rowsInUnallocatedBases;
    Vector<Vector<LayoutID>> m_layoutsByDepth;
};

struct SlotPlanContext {
    bool isCompiledObject(LayoutID id) const { return layoutStorage[id.index()] == LayoutStorage::CompiledObject; }
    bool isEnvironment(LayoutID id) const { return layoutStorage[id.index()] == LayoutStorage::Environment; }
    bool isMadeSeldom(LayoutID id) const { return layoutsMadeSeldom.get(id.index()); }

    void forEachHolder(PropertyKeyID key, LayoutSetID receivers, NOESCAPE const Invocable<void(LayoutID, SlotRow)> auto& functor) const
    {
        for (LayoutID member : program.at(receivers).seen().predicted()) {
            if (!isCompiledObject(member))
                continue;
            if (std::optional<uint32_t> field = program.at(member).findField(key))
                functor(member, rows.rowOf(member, *field));
        }
    }

    const Program& program;
    const SlotPlanInputs& inputs;
    BuildContext& build;
    const CellSizeClasses& sizeClasses;
    const SlotRows& rows;
    std::span<const LayoutStorage> layoutStorage;
    bool usesWideReferences;
    const InheritedRows& inheritedRows;
    BitVector layoutsMadeSeldom { };
};

struct SlotAccessKey {
    PropertyKeyID key;
    LayoutSetID receivers;
    bool isStore { false };

    friend auto operator<=>(const SlotAccessKey&, const SlotAccessKey&) = default;
};

struct SlotAccessUse {
    SlotAccessKey access;
    uint32_t sites { 0 };
    SiteWeight weight { 0 };
    SiteHeat fillHeat { 0 };
    uint32_t storesWithoutAllocation { 0 };
    uint32_t storesIntoFilledSlots { 0 };
    uint32_t storesIntoNewObjects { 0 };
};

struct SlotRowUse {
    Expected<bool> isStoredSomewhere() const { return fillHeat.predicted() || initializingSites || storesOfConstructorWithValue || storesOfUndefined; }
    Expected<bool> isLikelyFilled() const { return isFilledBesideCreation().predicted() || (holderHeat.predicted() && (fillHeat >= holderHeat * 0.4999).predicted()); }
    Expected<bool> isFilledBesideCreation() const { return storesOfConstructorWithValue || (isRequiredByDeclaringType && isStoredSomewhere().predicted()); }

    uint32_t accessSites { 0 };
    SiteWeight accessWeight { 0 };
    SiteHeat fillHeat { 0 };
    SiteHeat holderHeat { 0 };
    Expected<bool> hasHoldersMadeInLoop { false };
    SiteHeat heatFilledBesideCreation { 0 };
    uint32_t initializingSites { 0 };
    uint32_t initializingSitesOfOwnLayout { 0 };
    uint32_t sitesInitializingWithValue { 0 };
    uint32_t storesOfConstructorWithValue { 0 };
    uint32_t storesOfUndefined { 0 };
    bool isRequiredByDeclaringType { false };
    bool isOptionalInTypeOfThis { false };
    uint32_t environmentUses { 0 };
    uint32_t storesWithoutAllocation { 0 };
    uint32_t storesIntoFilledSlots { 0 };
    uint32_t storesIntoNewObjects { 0 };
    uint32_t privateDefinitionsByName { 0 };
    bool isFilledBySomeCopy { false };
};

struct LayoutUse {
    uint32_t allocationSites { 0 };
    SiteHeat creationHeat { 0 };
    Expected<bool> isMadeInLoop { false };
};

struct VariableUse {
    bool isNamedByCode { false };
    bool hasUncheckedRead { false };
    bool isUsedBeforeProgramEntry { false };
};

struct RowsStoredTogether {
    SlotRow first { 0 };
    SlotRow second { 0 };
    SiteWeight weight { 0 };
};

struct SlotUses {
    Vector<SlotAccessUse> accesses;
    // The heaviest first.
    Vector<RowsStoredTogether> rowsStoredTogether;
    Vector<SlotRowUse> rows;
    Vector<LayoutUse> layouts;
    Vector<VariableUse> variables;
    Vector<SiteWeight> keyWeights;
    FastBitVector keysNamed;
    FastBitVector keysDefinedByDescriptor;
    FastBitVector stringsNamed;
    BitVector functionsWhoseNewClosuresAreStoredInto;
};

class JoinedRows {
public:
    struct Access {
        uint32_t firstRow { 0 };
        uint32_t numberOfRows { 0 };
        SiteWeight weight { 0 };
    };

    struct Set {
        uint32_t firstRow { 0 };
        uint32_t numberOfRows { 0 };
        uint32_t firstAccess { 0 };
        uint32_t numberOfAccesses { 0 };
        SiteWeight weight { 0 };
    };

    JoinedRows(const SlotPlanContext&, const SlotUses&);

    // Heaviest first.
    std::span<const Set> sets() const LIFETIME_BOUND { return m_sets.span(); }
    // The rows in compiled objects, in ascending order.
    std::span<const SlotRow> rowsOf(const Set& joined) const LIFETIME_BOUND { return m_rows.span().subspan(joined.firstRow, joined.numberOfRows); }
    std::span<const Access> accessesOf(const Set& joined) const LIFETIME_BOUND { return m_accesses.span().subspan(joined.firstAccess, joined.numberOfAccesses); }
    std::span<const SlotRow> rowsOf(const Access& access) const LIFETIME_BOUND { return m_rowsOfAccesses.span().subspan(access.firstRow, access.numberOfRows); }

private:
    void collectAccesses(const SlotPlanContext&, const SlotUses&);

    Vector<Set> m_sets;
    Vector<SlotRow> m_rows;
    Vector<Access> m_accesses;
    Vector<SlotRow> m_rowsOfAccesses;
};

struct SlotClaim {
    bool hasStorage { false };
    bool mayBeOutOfLine { false };
    bool existsAtCreation { false };
    bool isInitializedWithValue { false };
    bool isSetAtCreation { false };
    bool needsAbsentState { false };
    bool needsWideReference { false };
    // The reference is wide because few such objects are created. Next to a layout that keeps it narrow, it is narrow too.
    bool isWideByChoice { false };
    bool deferredClosureCapturesObject { false };
    bool deferredClosureCapturesEnvironment { false };
    SlotRepresentation representation { SlotRepresentation::JSValue };
    SlotRepresentation outOfLineRepresentation { SlotRepresentation::JSValue };
    FunctionID deferredFunction;
};

struct RepresentationChoice {
    bool prefersInt32 { false };
    bool holdsWideReferences { false };
};

struct SharedLocation {
    Vector<SlotRow, 4> rows;
    SiteWeight weight { 0 };
};

struct PackedLayouts {
    Vector<PlacedSlot> slots;
    Vector<uint32_t> sizes;
    Vector<uint32_t> inlineLimits;
    Vector<uint32_t> extensionSizes;
    Vector<std::optional<uint32_t>> isEscapedOffsets;
    Vector<Vector<ReservedBytes>> bytesKeptZero;
    Vector<SharedLocation> sharedLocations;
    Vector<uint32_t> sharedLocationOfRow;
    // Rows whose bytes another layout keeps zero, so they stay where they are and as wide as they are.
    BitVector rowsWithEmptySlotElsewhere;
};

struct EmptySlotRoom {
    ScopedLambda<bool(SlotRow)> hasLocation;
    ScopedLambda<bool(ReservedBytes, LayoutID)> isFree;
    ScopedLambda<void(ReservedBytes, LayoutID)> take;
};

enum class WidenedRepresentations : uint8_t {
    Numbers,
    References,
    // The table of fields, which answers a read by name, has a form for 32 bits and none for 8 or 16.
    SmallIntegers,
};

using OccupiedBytes = Vector<uint64_t, 4>;

constexpr uint64_t bytesAt(uint32_t offset, uint32_t size)
{
    return ((1ull << size) - 1) << (offset % 64);
}

constexpr bool isSlotStore(Opcode opcode) { return storedOf(opcode).has_value(); }
inline const Field& declaredFieldAt(const Program& program, FieldReference reference) { return fieldAt(program, declarationOf(program, reference)); }

bool everyAllocationClassCannot(const Program&, const FlowResults&, LayoutID, OptionSet<ObjectMutation>);
bool someAllocationClassMay(const Program&, const FlowResults&, LayoutID, OptionSet<ObjectMutation>);
bool keysAreNeverEnumerated(const Program&, const FlowResults&, LayoutID);
bool mayHaveUnorderedShape(const Program&, const FlowResults&, LayoutID);
bool declaresPrivateName(const Layout&);
bool isDescribedToRunTime(const Program&, const FlowResults&, ModuleID exporter, const ModuleExport&);

Vector<LayoutStorage> chooseLayoutStorage(const Program&, const SlotPlanInputs&, BuildContext&);
JS_EXPORT_PRIVATE BitVector chooseKeysWithColumn(const SlotPlanContext&, const SlotUses&);
JS_EXPORT_PRIVATE SlotUses collectSlotUses(const SlotPlanContext&);
void shareUsesOfInheritedRows(const InheritedRows&, SlotUses&);
JS_EXPORT_PRIVATE void dumpFillEstimates(PrintStream&, const Program&, const SlotUses&);
BitVector findZeroEncodingCandidates(const SlotPlanContext&);
void findUncheckedVariableReads(FunctionAnalyses&, const BitVector& candidates, Vector<VariableID>& unchecked);

Type contentsOfSharedSlot(const Type&, const Type&);
SlotRepresentation representationFor(const SlotPlanContext&, const Type& typeWithAbsent, RepresentationChoice);
SlotRepresentation outOfLineRepresentationFor(const SlotPlanContext&, const Type& typeWithAbsent, bool holdsWideReferences);
bool holdsWideReferences(const SlotPlanContext&, LayoutID);
Vector<SlotClaim> chooseSlotRepresentations(const SlotPlanContext&, const SlotUses&, const JoinedRows&);

PackedLayouts packObjects(const SlotPlanContext&, const SlotUses&, const JoinedRows&, std::span<const SlotClaim>);
void packEnvironments(const SlotPlanContext&, const SlotUses&, std::span<const SlotClaim>, PackedLayouts&);
OccupiedBytes occupiedBytesOf(const SlotPlanContext&, const PackedLayouts&, LayoutID);
void widenSlotsIntoPadding(const SlotPlanContext&, const SlotUses&, const JoinedRows&, std::span<const SlotClaim>, WidenedRepresentations, PackedLayouts&);
void reserveEmptySlots(const SlotPlanContext&, const SlotUses&, PackedLayouts&, const EmptySlotRoom&);
void reserveEmptySlotsInPadding(const SlotPlanContext&, const SlotUses&, PackedLayouts&);

Vector<PlannedVariable> planVariables(const SlotPlanContext&, const SlotUses&);
Vector<ClosureForm> chooseClosureForms(const SlotPlanContext&, const SlotUses&);
Vector<ClassCreationPlan> planClassCreation(const SlotPlanContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
