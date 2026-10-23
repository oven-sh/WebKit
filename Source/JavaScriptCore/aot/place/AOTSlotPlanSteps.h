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
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <compare>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

using SlotRow = uint32_t;

inline constexpr uint32_t noSharedLocation = UINT32_MAX;
inline constexpr uint32_t referencesBeyondCreation = 4;
inline constexpr uint32_t maxInlineSize = 128;
inline constexpr uint32_t maxInlineSizeAtCreation = 512;
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

// Along a proven chain of bases the rows of one field, and those of a hidden word, form a family: one claim, one location.
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
    // The number of proven bases.
    uint32_t depthOf(LayoutID id) const { return m_depths[id.index()]; }
    SlotRow declaringRowOf(SlotRow row) const { return m_declaringRow[row]; }
    // By the depth of the declaring layout, then by declaring row.
    std::span<const Family> families() const LIFETIME_BOUND { return m_families.span(); }
    // The rows in compiled objects. The declaring layout is among them only where it is allocated.
    std::span<const SlotRow> rowsOf(const Family& family) const LIFETIME_BOUND { return m_rows.span().subspan(family.firstRow, family.numberOfRows); }
    // The rows in bases that are never allocated themselves. Nothing is placed for them, but what the constructor of such a base stores is counted on them.
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

    const Program& program;
    const SlotPlanInputs& inputs;
    BuildContext& build;
    const CellSizeClasses& sizeClasses;
    const SlotRows& rows;
    std::span<const LayoutStorage> layoutStorage;
    bool usesWideReferences;
    const InheritedRows& inheritedRows;
    // What saves memory at the price of instructions is for the layouts that are not among them. Known once the uses are collected.
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
    // Of the stores of something else than the constant undefined, in heat without a limit.
    double fillHeat { 0 };
    uint32_t storesWithoutAllocation { 0 };
    uint32_t storesIntoFilledSlots { 0 };
    uint32_t storesIntoNewObjects { 0 };
};

struct StoreByNameUse {
    PropertyKeyID key;
    LayoutSetID receivers;
    double heat { 0 };
};

struct SlotRowUse {
    // Whether any code that was seen gives the field something, be it undefined.
    bool isStoredSomewhere() const { return fillHeat || initializingSites || storesOfConstructorWithValue || storesOfUndefined; }
    // An estimate of the share of objects that get the field: how often it is stored against how often its holders are made.
    bool isLikelyFilled() const { return isFilledBesideCreation() || (holderHeat && fillHeat >= holderHeat * 0.4999); }
    bool isFilledBesideCreation() const { return storesOfConstructorWithValue || (isRequiredByDeclaringType && isStoredSomewhere()); }

    SiteWeight accessWeight { 0 };
    // In heat without a limit: by the estimate half the functions of a large program are entered more often than maxSiteActivations, and limited weights say nothing about one another.
    double fillHeat { 0 };
    double holderHeat { 0 };
    bool hasHoldersMadeInLoop { false };
    // How often an object of the layout is made at a place whose function goes on to store a value into the field.
    double heatFilledBesideCreation { 0 };
    uint32_t initializingSites { 0 };
    uint32_t initializingSitesOfOwnLayout { 0 };
    uint32_t sitesInitializingWithValue { 0 };
    uint32_t storesOfConstructorWithValue { 0 };
    // Stores of the constant undefined behind the allocation: they say that the field exists and fill nothing.
    uint32_t storesOfUndefined { 0 };
    bool isRequiredByDeclaringType { false };
    // The field of a constructor function is required because the constructor stores it, be it undefined. The type that `this` is declared to have says whether the program counts on a value.
    bool isOptionalInTypeOfThis { false };
    uint32_t environmentUses { 0 };
    uint32_t storesWithoutAllocation { 0 };
    uint32_t storesIntoFilledSlots { 0 };
    // Slot stores into an object of exactly this layout that the same function has allocated.
    uint32_t storesIntoNewObjects { 0 };
    uint32_t privateDefinitionsByName { 0 };
};

struct LayoutUse {
    uint32_t allocationSites { 0 };
    double creationHeat { 0 };
    // By what the code itself shows: see findFunctionsThatRunInLoops. An event counts for far more than a loop in the heat of a run, so the heat does not say it.
    bool isMadeInLoop { false };
};

struct VariableUse {
    bool isNamedByCode { false };
    bool hasUncheckedRead { false };
    bool isUsedBeforeProgramEntry { false };
};

// Two rows of one layout that a function stores one after the other into one object: an object that has the one mostly has the other.
struct RowsStoredTogether {
    SlotRow first { 0 };
    SlotRow second { 0 };
    SiteWeight weight { 0 };
};

struct SlotUses {
    Vector<SlotAccessUse> accesses;
    Vector<StoreByNameUse> storesByName;
    // The heaviest first.
    Vector<RowsStoredTogether> rowsStoredTogether;
    Vector<SlotRowUse> rows;
    Vector<LayoutUse> layouts;
    Vector<VariableUse> variables;
    Vector<SiteWeight> keyWeights;
    BitVector functionsWhoseNewClosuresAreStoredInto;
};

// The fields that one access reaches under one key in several layouts are joined, and the rows of their families with them. Joined rows want one claim and one location.
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
        // Of the accesses that reach two rows or more.
        SiteWeight weight { 0 };
    };

    JoinedRows(const SlotPlanContext&, const SlotUses&);

    // Heaviest first.
    std::span<const Set> sets() const LIFETIME_BOUND { return m_sets.span(); }
    // The rows in compiled objects, in ascending order.
    std::span<const SlotRow> rowsOf(const Set& joined) const LIFETIME_BOUND { return m_rows.span().subspan(joined.firstRow, joined.numberOfRows); }
    // Every access to a row of the set, the reads and the stores through the same receivers as one.
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
};

struct SlotPlanParts {
    Vector<PlannedLayout> layouts;
    Vector<PlannedVariable> variables;
    Vector<ClosureForm> closureForms;
    Vector<ClassCreationPlan> classCreation;
    FormsExpectedOfNames formsExpectedOfNames;
    BitVector keysWithColumn;
};

using OccupiedBytes = Vector<uint64_t, 4>;

constexpr uint64_t bytesAt(uint32_t offset, uint32_t size)
{
    return ((1ull << size) - 1) << (offset % 64);
}

constexpr bool isSlotStore(Opcode opcode) { return storedOf(opcode).has_value(); }

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

// What a slot holds that several fields share, as far as its representation depends on it: no object is told from another.
Type contentsOfSharedSlot(const Type&, const Type&);
SlotRepresentation representationFor(const SlotPlanContext&, const Type& typeWithAbsent, RepresentationChoice);
SlotRepresentation outOfLineRepresentationFor(const SlotPlanContext&, const Type& typeWithAbsent, bool holdsWideReferences);
bool holdsWideReferences(const SlotPlanContext&, LayoutID);
Vector<SlotClaim> chooseSlotRepresentations(const SlotPlanContext&, const SlotUses&, const JoinedRows&);

PackedLayouts packObjects(const SlotPlanContext&, const SlotUses&, const JoinedRows&, std::span<const SlotClaim>);
void packEnvironments(const SlotPlanContext&, const SlotUses&, std::span<const SlotClaim>, PackedLayouts&);
OccupiedBytes occupiedBytesOf(const SlotPlanContext&, const PackedLayouts&, LayoutID);
void widenSlotsIntoPadding(const SlotPlanContext&, const SlotUses&, std::span<const SlotClaim>, WidenedRepresentations, PackedLayouts&);
void reserveEmptySlots(const SlotPlanContext&, const SlotUses&, PackedLayouts&, const EmptySlotRoom&);
void reserveEmptySlotsInPadding(const SlotPlanContext&, const SlotUses&, PackedLayouts&);

Vector<PlannedVariable> planVariables(const SlotPlanContext&, const SlotUses&);
Vector<ClosureForm> chooseClosureForms(const SlotPlanContext&, const SlotUses&);
Vector<ClassCreationPlan> planClassCreation(const SlotPlanContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
