/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocation.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTRepresentation.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// What the layouts that hold the field say. The rows of a family share a claim, so each answer is the one that suits every member.
struct MemberFacts {
    void meetWith(const MemberFacts& other)
    {
        mayReachUnknownJavaScript |= other.mayReachUnknownJavaScript;
        isFilledByNativeCode |= other.isFilledByNativeCode;
        holdsWideReferences |= other.holdsWideReferences;
        needsWideReference |= other.needsWideReference;
        mayHaveUnorderedShape |= other.mayHaveUnorderedShape;
        canHaveExtension &= other.canHaveExtension;
    }

    bool mayReachUnknownJavaScript { false };
    bool isFilledByNativeCode { false };
    bool holdsWideReferences { false };
    bool needsWideReference { false };
    bool mayHaveUnorderedShape { false };
    bool canHaveExtension { true };
};

struct FieldInputs {
    FieldReference reference;
    const Layout& layout;
    const Field& declaration;
    const FieldFlow& flow;
    const Type& type;
    const SlotRowUse& use;
    bool isCreatedByStore;
    MemberFacts members;
};

} // anonymous namespace

#if USE(BIGINT32)
static constexpr uint16_t primitivesAlwaysInCells = cellPrimitives & ~Primitive::BigInt;
#else
static constexpr uint16_t primitivesAlwaysInCells = cellPrimitives;
#endif

static constexpr uint16_t markerPrimitives = Primitive::UndefinedOrNull | Primitive::Absent;

bool holdsWideReferences(const SlotPlanContext& context, LayoutID id)
{
    return context.usesWideReferences || context.program.at(id).kind == LayoutKind::CoroutineFrame;
}

static bool needsWideReference(const SlotPlanContext& context, FieldReference field)
{
    return holdsWideReferences(context, field.layout) || context.inputs.frameAllocation.mayHoldFramePointer(field);
}

static bool canHaveExtension(const SlotPlanContext& context, LayoutID id)
{
    return context.isCompiledObject(id) && !context.inputs.frameAllocation.mayBeInFrame(id);
}

static bool isCompressible(const Type& type)
{
    if (!type.hasCells() || type.primitives & ~(markerPrimitives | primitivesAlwaysInCells))
        return false;
    return !type.opaque || holdsCells(representationOf(*type.opaque));
}

Type contentsOfSharedSlot(const Type& a, const Type& b)
{
    ASSERT(a.isJavaScriptValue() && b.isJavaScriptValue());
    Type contents = Type::fromPrimitives(a.primitives | b.primitives);
    contents.integers = a.integers.unionWith(b.integers);
    contents.anyObject = a.hasObjects() || b.hasObjects();
    return contents;
}

SlotRepresentation representationFor(const SlotPlanContext& context, const Type& type, RepresentationChoice choice)
{
    SlotRepresentation representation = defaultSlotRepresentationOf(type);
    if (representation == SlotRepresentation::JSValue && type.hasOnlyPrimitives(markerPrimitives | Primitive::Boolean))
        return SlotRepresentation::OptionalBoolean;
    if (choice.prefersInt32 && representation == SlotRepresentation::OptionalInt64 && type.integers.isSubsetOf({ minInlineInt32, INT32_MAX }))
        return SlotRepresentation::OptionalInt32;
    if (choice.prefersInt32 && widthOf(representation) == 8 && type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | markerPrimitives))
        return SlotRepresentation::Int32OrDouble;
    if (!choice.holdsWideReferences && !context.usesWideReferences && isCompressible(type)) {
        if (representation == SlotRepresentation::Cell)
            return SlotRepresentation::Cell32;
        if (representation == SlotRepresentation::JSValue)
            return SlotRepresentation::OptionalCell32;
        if (representation == SlotRepresentation::LiteralString)
            return SlotRepresentation::LiteralString32;
    }
    return representation;
}

SlotRepresentation outOfLineRepresentationFor(const SlotPlanContext& context, const Type& typeWithAbsent, bool holdsWideReferences)
{
    SlotRepresentation representation = representationFor(context, typeWithAbsent, { .prefersInt32 = false, .holdsWideReferences = holdsWideReferences });
    return representsAbsentAsZero(representation) ? representation : SlotRepresentation::JSValue;
}

static SlotRepresentation representationOfField(const SlotPlanContext& context, const Type& type, bool needsAbsentState, RepresentationChoice choice)
{
    if (needsAbsentState)
        return representationFor(context, type.withPrimitives(Primitive::Absent), choice);
    return representationFor(context, type, choice);
}

static bool needsAbsentState(const FieldInputs& field)
{
    constexpr OptionSet<FieldStability> everyStability { FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined, FieldStability::IsNeverStoredWithOtherType };
    bool isAlwaysFilled = field.flow.occupancy == Occupancy::AlwaysFilled;
    switch (field.layout.kind) {
    case LayoutKind::CoroutineFrame:
        return false;
    case LayoutKind::Environment:
        return field.flow.location.initialization == InitializationState::Unknown || !field.flow.stability.containsAll(everyStability);
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        return !isAlwaysFilled
            || field.isCreatedByStore
            || !field.flow.stability.containsAll(everyStability)
            || field.members.mayReachUnknownJavaScript;
    case LayoutKind::Closure:
    case LayoutKind::ModuleNamespace:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isFilledByNativeCode(const Layout& layout)
{
    return layout.flow.observedUses.contains(ObservedUse::ReachesTypedParse);
}

static bool hasStorage(const SlotPlanContext& context, const FieldInputs& field)
{
    if (context.isEnvironment(field.reference.layout))
        return !isRemovedField(context.program, field.reference);
    if (!context.isCompiledObject(field.reference.layout))
        return false;
    constexpr OptionSet<FieldFlags> privateNameFlags { FieldFlags::IsPrivate, FieldFlags::IsBrand };
    if (field.declaration.flags.containsAny(privateNameFlags))
        return true;

    bool isNeverWritten = isNeverStored(context.program, field.reference) && !field.use.initializingSites;
    if (isNeverWritten && (field.declaration.flags.contains(FieldFlags::IsMethod) || !field.members.isFilledByNativeCode))
        return false;
    return !isRemovedField(context.program, field.reference);
}

// Such a slot has no absent state and needs none: what would empty it makes the object an ordinary one.
static std::optional<FunctionID> deferredFunctionOf(const SlotPlanContext& context, const FieldInputs& field)
{
    const LocationFlow& location = field.flow.location;
    if (!context.isCompiledObject(field.reference.layout) || field.declaration.flags.contains(FieldFlags::IsPrivate))
        return std::nullopt;
    if (location.deferral == ClosureDeferral::Unknown || !location.deferredFunction)
        return std::nullopt;
    if (field.members.needsWideReference || field.isCreatedByStore || field.use.storesOfOtherThanDeferredClosure)
        return std::nullopt;
    return location.deferredFunction;
}

static bool prefersInt32(const SlotPlanContext& context, const FieldInputs& field, bool needsAbsentState)
{
    if (field.type.isInt32OrNullish() && field.type.integers.isSubsetOf({ minInlineInt32, INT32_MAX }))
        return true;
    if (!isPredictedInt32(context.program, field.declaration) || !field.members.canHaveExtension || field.members.holdsWideReferences)
        return false;
    if (field.use.wideStoresWithoutAllocation)
        return false;
    bool isOnlyUsedAsDouble = !field.use.integerUses && field.use.doubleUses;
    return needsAbsentState || !field.type.isNumber() || !(field.use.doubleStores || isOnlyUsedAsDouble);
}

// A field that an object lists before it has an Extension reads as undefined. An unordered object has no list, so its slots alone say what it has.
static bool mayBeOutOfLine(const FieldInputs& field, const SlotClaim& claim)
{
    if (claim.existsAtCreation && field.members.mayHaveUnorderedShape)
        return false;
    if (claim.representation == SlotRepresentation::DeferredClosure || field.use.storesWithoutAllocation)
        return false;
    if (claim.existsAtCreation && field.use.storesIntoFilledSlots)
        return false;
    if (field.declaration.flags.contains(FieldFlags::IsImmutable) || field.flow.location.stores == StoreCount::AtMostOnce)
        return false;
    bool mayBeUndefined = field.type.primitives & Primitive::Undefined;
    if (field.declaration.flags.contains(FieldFlags::IsRequired) && !mayBeUndefined)
        return false;
    if (claim.existsAtCreation && (!mayBeUndefined || claim.isInitializedWithValue))
        return false;
    return field.members.canHaveExtension;
}

static SlotClaim claimFor(const SlotPlanContext& context, const FieldInputs& field)
{
    SlotClaim claim;
    claim.hasStorage = hasStorage(context, field);
    if (!claim.hasStorage)
        return claim;

    const LocationFlow& location = field.flow.location;
    bool isAlwaysFilledByNativeCode = field.members.isFilledByNativeCode && field.declaration.flags.contains(FieldFlags::IsRequired);
    // In a validating build the slot of a binding can say that nothing was stored yet, so that a read which is proven to come after a store can be tested.
    claim.needsAbsentState = needsAbsentState(field) || (context.build.validates() && field.layout.kind == LayoutKind::Environment);
    claim.isSetAtCreation = location.initialization == InitializationState::SetAtCreation;
    claim.existsAtCreation = field.use.initializingSites || claim.isSetAtCreation || isAlwaysFilledByNativeCode;
    claim.isInitializedWithValue = field.use.sitesInitializingWithValue || (claim.isSetAtCreation && !field.use.initializingSites) || isAlwaysFilledByNativeCode;

    if (auto function = deferredFunctionOf(context, field)) {
        claim.needsAbsentState = false;
        claim.representation = SlotRepresentation::DeferredClosure;
        claim.deferredFunction = *function;
        claim.deferredClosureCapturesObject = location.deferral == ClosureDeferral::CapturesObject;
        return claim;
    }

    claim.needsWideReference = field.members.needsWideReference;
    claim.representation = representationOfField(context, field.type, claim.needsAbsentState, {
        .prefersInt32 = prefersInt32(context, field, claim.needsAbsentState),
        .holdsWideReferences = claim.needsWideReference,
    });
    claim.representationWithoutOverflowArea = representationOfField(context, field.type, claim.needsAbsentState, { .prefersInt32 = false, .holdsWideReferences = claim.needsWideReference });
    claim.mayBeOutOfLine = mayBeOutOfLine(field, claim);
    if (claim.mayBeOutOfLine)
        claim.outOfLineRepresentation = outOfLineRepresentationFor(context, field.type.withPrimitives(Primitive::Absent), claim.needsWideReference);
    return claim;
}

static const Field& declaredFieldAt(const Program& program, FieldReference reference)
{
    return fieldAt(program, declarationOf(program, reference));
}

static void demoteLoneInt32Slot(const SlotPlanContext& context, LayoutID id, std::span<SlotClaim> fieldClaims)
{
    unsigned numberOfInt32Slots = 0;
    FieldIndex lastInt32Slot = 0;
    for (FieldIndex index = 0; index < fieldClaims.size(); ++index) {
        if (!fieldClaims[index].hasStorage || fieldClaims[index].representation != SlotRepresentation::Int32OrDouble)
            continue;
        ++numberOfInt32Slots;
        lastInt32Slot = index;
    }
    if (numberOfInt32Slots != 1)
        return;

    SlotClaim& claim = fieldClaims[lastInt32Slot];
    const Type& type = context.program.at(declaredFieldAt(context.program, { id, lastInt32Slot }).declared.type);
    claim.representation = representationOfField(context, type, claim.needsAbsentState, {
        .prefersInt32 = false,
        .holdsWideReferences = holdsWideReferences(context, id),
    });
    claim.representationWithoutOverflowArea = claim.representation;
}

static SlotClaim claimForExtraPropertiesWord(const SlotPlanContext& context, LayoutID id)
{
    constexpr OptionSet<ObjectMutation> mutationsCreatingExtraProperties { ObjectMutation::AddProperty, ObjectMutation::RedefineProperty, ObjectMutation::StoreOfOtherType };
    SlotClaim claim;
    claim.hasStorage = someAllocationClassMay(context.program, context.inputs.flow, id, mutationsCreatingExtraProperties);
    claim.representation = hiddenReferenceRepresentation(context.usesWideReferences);
    return claim;
}

static SlotRow declaringRowOf(const SlotPlanContext& context, FieldReference reference)
{
    FieldReference declaration = declarationOf(context.program, reference);
    return context.rows.rowOf(declaration.layout, declaration.field);
}

// The store that creates a field records the key, so it has to find the slot absent. Only an allocation that lists the field records the key itself.
// A base class constructor allocates with NewObject. For a subclass and for a function called with new the run time allocates, and it lists nothing.
// The run time's definition of a private name throws where the field exists, so it has to find the slot absent whoever enumerates.
// The answer is by declaring row, so that a class and its subclasses agree on the slot.
static BitVector findFieldsCreatedByStores(const SlotPlanContext& context, const SlotUses& uses)
{
    const Program& program = context.program;
    BitVector declaringRows(context.rows.count());
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const Layout& layout = program.at(id);
        for (FieldIndex field = 0; field < layout.fields.size(); ++field) {
            if (uses.rows[context.rows.rowOf(id, field)].privateDefinitionsByName)
                declaringRows.quickSet(declaringRowOf(context, { id, field }));
        }
        if (!context.isCompiledObject(id) || !layout.constructor || keysAreNeverEnumerated(program, context.inputs.flow, id))
            continue;

        bool isAllocatedByRunTime = program.at(layout.constructor).kind != FunctionKind::BaseConstructor;
        uint32_t allocationSites = uses.layouts[index].allocationSites;
        for (FieldIndex field = 0; field < layout.fields.size(); ++field) {
            if (isAllocatedByRunTime || uses.rows[context.rows.rowOf(id, field)].initializingSites != allocationSites)
                declaringRows.quickSet(declaringRowOf(context, { id, field }));
        }
    }
    return declaringRows;
}

static MemberFacts memberFactsOf(const SlotPlanContext& context, FieldReference reference)
{
    LayoutID id = reference.layout;
    return {
        .mayReachUnknownJavaScript = !everyAllocationClassCannot(context.program, context.inputs.flow, id, ObjectMutation::ReachUnknownJavaScript),
        .isFilledByNativeCode = isFilledByNativeCode(context.program.at(id)),
        .holdsWideReferences = holdsWideReferences(context, id),
        .needsWideReference = needsWideReference(context, reference),
        .mayHaveUnorderedShape = mayHaveUnorderedShape(context.program, context.inputs.flow, id),
        .canHaveExtension = canHaveExtension(context, id),
    };
}

static SlotClaim claimFor(const SlotPlanContext& context, const SlotUses& uses, const BitVector& fieldsCreatedByStores, FieldReference reference, const MemberFacts& members)
{
    const Program& program = context.program;
    const Field& declaration = declaredFieldAt(program, reference);
    return claimFor(context, {
        .reference = reference,
        .layout = program.at(reference.layout),
        .declaration = declaration,
        .flow = flowOf(program, reference),
        .type = program.at(declaration.declared.type),
        .use = uses.rows[context.rows.rowOf(reference.layout, reference.field)],
        .isCreatedByStore = fieldsCreatedByStores.quickGet(declaringRowOf(context, reference)),
        .members = members,
    });
}

static void claimRowsOf(const SlotPlanContext& context, const SlotUses& uses, const BitVector& fieldsCreatedByStores, LayoutID id, std::span<SlotClaim> claims)
{
    size_t numberOfFields = context.program.at(id).fields.size();
    SlotRow firstRow = context.rows.firstRowOf(id);
    for (FieldIndex index = 0; index < numberOfFields; ++index) {
        FieldReference reference { id, index };
        claims[firstRow + index] = claimFor(context, uses, fieldsCreatedByStores, reference, memberFactsOf(context, reference));
    }
    if (!context.isCompiledObject(id))
        return;

    demoteLoneInt32Slot(context, id, claims.subspan(firstRow, numberOfFields));
    claims[context.rows.extraPropertiesWordOf(id)] = claimForExtraPropertiesWord(context, id);
}

static FieldReference fieldAtRow(const SlotPlanContext& context, SlotRow row)
{
    LayoutID id = context.rows.layoutOf(row);
    return { id, row - context.rows.firstRowOf(id) };
}

// The declaring row gets the claim too: its layout chooses between the cell and the Extension for the family, allocated or not.
static void claimRowsOf(const SlotPlanContext& context, const SlotUses& uses, const BitVector& fieldsCreatedByStores, const InheritedRows::Family& family, std::span<SlotClaim> claims)
{
    std::span<const SlotRow> rows = context.inheritedRows.rowsOf(family);
    SlotClaim claim;
    if (context.rows.isHiddenWord(family.declaringRow)) {
        for (SlotRow row : rows) {
            LayoutID id = context.rows.layoutOf(row);
            if (row == context.rows.extraPropertiesWordOf(id) && !claim.hasStorage)
                claim = claimForExtraPropertiesWord(context, id);
        }
    } else {
        MemberFacts members = memberFactsOf(context, fieldAtRow(context, rows[0]));
        for (SlotRow row : rows.subspan(1))
            members.meetWith(memberFactsOf(context, fieldAtRow(context, row)));
        claim = claimFor(context, uses, fieldsCreatedByStores, fieldAtRow(context, rows[0]), members);
    }
    for (SlotRow row : rows)
        claims[row] = claim;
    claims[family.declaringRow] = claim;
}

namespace {

// At an access that knows the layout a value keeps its form as long as the slot's representation stays of one kind.
enum class SharedValueKind : uint8_t {
    Integer,
    Number,
    Boolean,
    Reference,
    Boxed,
    LiteralString,
    OnlyMarkers,
};
constexpr unsigned numberOfSharedValueKinds = 7;

// The rows of a family, which have one claim, or a row that is in no family.
struct ClaimUnit {
    std::span<const SlotRow> rows;
    const Type* type { nullptr };
    SharedValueKind kind { SharedValueKind::Boxed };
};

struct SharedClaim {
    SlotRepresentation representation { SlotRepresentation::JSValue };
    SlotRepresentation outOfLineRepresentation { SlotRepresentation::JSValue };
    bool needsAbsentState { false };
    bool needsWideReference { false };
};

class ClaimSharer {
public:
    ClaimSharer(const SlotPlanContext&, const JoinedRows&, std::span<SlotClaim>);

    void run();

private:
    const SlotClaim& claimOf(const ClaimUnit& unit) const { return m_claims[unit.rows[0]]; }

    void computeSpareBytes();
    void collectUnits(const JoinedRows::Set&);
    SharedClaim claimFor(std::span<const ClaimUnit>) const;
    bool isWorthSharing(const JoinedRows::Set&, std::span<const ClaimUnit>, SlotRepresentation) const;
    bool hasSpareBytesFor(const ClaimUnit&, SlotRepresentation) const;
    void apply(const SharedClaim&, const ClaimUnit&);
    bool tryShare(const JoinedRows::Set&, Vector<ClaimUnit, 8>&&);
    void share(const JoinedRows::Set&);

    const SlotPlanContext& m_context;
    const JoinedRows& m_joinedRows;
    std::span<SlotClaim> m_claims;
    Vector<uint32_t> m_spareBytes;
    Vector<SlotRow, 16> m_rowsByFamily;
    Vector<ClaimUnit, 8> m_units;
};

} // anonymous namespace

static std::optional<SharedValueKind> sharedValueKindOf(SlotRepresentation representation)
{
    switch (traitsOf(representation).valueKind) {
    case SlotValueKind::SignedInteger:
    case SlotValueKind::UnsignedInteger:
        return SharedValueKind::Integer;
    case SlotValueKind::Float:
    case SlotValueKind::Double:
    case SlotValueKind::Int32OrDouble:
        return SharedValueKind::Number;
    case SlotValueKind::Boolean:
        return SharedValueKind::Boolean;
    case SlotValueKind::Cell:
        return SharedValueKind::Reference;
    case SlotValueKind::Boxed:
        return SharedValueKind::Boxed;
    case SlotValueKind::LiteralString:
        return SharedValueKind::LiteralString;
    case SlotValueKind::DeferredClosure:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ClaimSharer::ClaimSharer(const SlotPlanContext& context, const JoinedRows& joinedRows, std::span<SlotClaim> claims)
    : m_context(context)
    , m_joinedRows(joinedRows)
    , m_claims(claims)
{
}

// What a cell of the layout's size class has beyond its fields. A shared representation is wider than a field's own only within these bytes, so no cell grows for it.
void ClaimSharer::computeSpareBytes()
{
    const SlotRows& rows = m_context.rows;
    uint32_t hiddenWordWidth = widthOf(hiddenReferenceRepresentation(m_context.usesWideReferences));
    m_spareBytes.fill(0, m_context.program.layouts.size());
    forEachIndexInParallel(m_context.build.parallelLoops(), m_spareBytes.size(), [&](unsigned index) {
        LayoutID id(index);
        if (!m_context.isCompiledObject(id))
            return;
        uint32_t bytes = offsetOfFirstField;
        bool hasOverflowAreas = false;
        for (SlotRow row = rows.firstRowOf(id); row < rows.firstRowOf(id) + rows.numberOfRowsOf(id); ++row) {
            if (!m_claims[row].hasStorage)
                continue;
            bytes += widthOf(m_claims[row].representation);
            hasOverflowAreas |= m_claims[row].representation == SlotRepresentation::Int32OrDouble;
        }
        if (hasOverflowAreas)
            bytes += hiddenWordWidth;
        m_spareBytes[index] = static_cast<uint32_t>(m_context.sizeClasses.cellSizeFor(bytes)) - bytes;
    });
}

void ClaimSharer::collectUnits(const JoinedRows::Set& joined)
{
    const Program& program = m_context.program;
    const InheritedRows& inheritedRows = m_context.inheritedRows;
    m_units.shrink(0);
    m_rowsByFamily.shrink(0);
    for (SlotRow row : m_joinedRows.rowsOf(joined)) {
        if (m_claims[row].hasStorage)
            m_rowsByFamily.append(row);
    }
    std::ranges::sort(m_rowsByFamily, { }, [&](SlotRow row) {
        return std::pair(inheritedRows.declaringRowOf(row), row);
    });
    std::span<const SlotRow> rows = m_rowsByFamily.span();
    for (size_t first = 0; first < rows.size();) {
        size_t end = first + 1;
        while (end < rows.size() && inheritedRows.declaringRowOf(rows[end]) == inheritedRows.declaringRowOf(rows[first]))
            ++end;
        const Type& type = program.at(declaredFieldAt(program, fieldAtRow(m_context, rows[first])).declared.type);
        auto kind = sharedValueKindOf(m_claims[rows[first]].representation);
        if (kind && type.isJavaScriptValue())
            m_units.append(ClaimUnit { rows.subspan(first, end - first), &type, type.hasOnlyPrimitives(markerPrimitives) ? SharedValueKind::OnlyMarkers : *kind });
        first = end;
    }
}

SharedClaim ClaimSharer::claimFor(std::span<const ClaimUnit> units) const
{
    SharedClaim shared;
    Type contents;
    bool everyUnitHasOverflowArea = true;
    for (const ClaimUnit& unit : units) {
        const SlotClaim& claim = claimOf(unit);
        contents = contentsOfSharedSlot(contents, *unit.type);
        shared.needsAbsentState |= claim.needsAbsentState;
        shared.needsWideReference |= claim.needsWideReference;
        everyUnitHasOverflowArea &= claim.representation == SlotRepresentation::Int32OrDouble;
    }
    bool fitsInt32Slot = contents.isInt32OrNullish() && contents.integers.isSubsetOf({ minInlineInt32, INT32_MAX });
    shared.representation = representationOfField(m_context, contents, shared.needsAbsentState, {
        .prefersInt32 = fitsInt32Slot || everyUnitHasOverflowArea,
        .holdsWideReferences = shared.needsWideReference,
    });
    shared.outOfLineRepresentation = outOfLineRepresentationFor(m_context, contents.withPrimitives(Primitive::Absent), shared.needsWideReference);
    return shared;
}

static bool changesForm(const ClaimUnit& unit, SlotRepresentation own, SlotRepresentation shared)
{
    if (unit.kind == SharedValueKind::OnlyMarkers)
        return false;
    return sharedValueKindOf(own) != sharedValueKindOf(shared) || (!canLeaveSlot(own) && canLeaveSlot(shared));
}

// One representation saves the accesses that reach fields with several a dispatch on the shape. Where it is of another kind than a field's own, or has an absent state that the field does without,
// it costs the accesses that reach only such fields.
bool ClaimSharer::isWorthSharing(const JoinedRows::Set& joined, std::span<const ClaimUnit> units, SlotRepresentation shared) const
{
    Vector<std::pair<SlotRow, const ClaimUnit*>, 16> unitsByRow;
    for (const ClaimUnit& unit : units) {
        for (SlotRow row : unit.rows)
            unitsByRow.append({ row, &unit });
    }
    std::ranges::sort(unitsByRow);

    SiteWeight weightSaved = 0;
    SiteWeight weightSpent = 0;
    for (const JoinedRows::Access& access : m_joinedRows.accessesOf(joined)) {
        const ClaimUnit* reached = nullptr;
        bool reachesSeveralRepresentations = false;
        bool reachesOnlyChangedForms = true;
        for (SlotRow row : m_joinedRows.rowsOf(access)) {
            auto entry = std::ranges::lower_bound(unitsByRow, row, { }, &std::pair<SlotRow, const ClaimUnit*>::first);
            if (entry == unitsByRow.end() || entry->first != row)
                continue;
            const ClaimUnit& unit = *entry->second;
            reachesSeveralRepresentations |= reached && claimOf(*reached).representation != claimOf(unit).representation;
            reachesOnlyChangedForms &= changesForm(unit, claimOf(unit).representation, shared);
            reached = &unit;
        }
        if (reachesSeveralRepresentations)
            weightSaved = saturatedSum(weightSaved, access.weight);
        else if (reached && reachesOnlyChangedForms)
            weightSpent = saturatedSum(weightSpent, access.weight);
    }
    return weightSaved >= weightSpent;
}

bool ClaimSharer::hasSpareBytesFor(const ClaimUnit& unit, SlotRepresentation representation) const
{
    uint32_t ownWidth = widthOf(claimOf(unit).representation);
    uint32_t width = widthOf(representation);
    return width <= ownWidth || std::ranges::all_of(unit.rows, [&](SlotRow row) {
        return m_spareBytes[m_context.rows.layoutOf(row).index()] >= width - ownWidth;
    });
}

void ClaimSharer::apply(const SharedClaim& shared, const ClaimUnit& unit)
{
    uint32_t ownWidth = widthOf(claimOf(unit).representation);
    uint32_t width = widthOf(shared.representation);
    auto applyTo = [&](SlotClaim& claim) {
        claim.representation = shared.representation;
        claim.needsAbsentState = shared.needsAbsentState;
        claim.needsWideReference = shared.needsWideReference;
        if (claim.mayBeOutOfLine)
            claim.outOfLineRepresentation = shared.outOfLineRepresentation;
    };
    for (SlotRow row : unit.rows) {
        if (width > ownWidth)
            m_spareBytes[m_context.rows.layoutOf(row).index()] -= width - ownWidth;
        applyTo(m_claims[row]);
    }
    applyTo(m_claims[m_context.inheritedRows.declaringRowOf(unit.rows[0])]);
}

bool ClaimSharer::tryShare(const JoinedRows::Set& joined, Vector<ClaimUnit, 8>&& units)
{
    while (units.size() >= 2) {
        SharedClaim shared = claimFor(units.span());
        bool removedUnit = units.removeAllMatching([&](const ClaimUnit& unit) {
            return !hasSpareBytesFor(unit, shared.representation);
        });
        if (removedUnit)
            continue;
        if (!isWorthSharing(joined, units.span(), shared.representation))
            return false;
        for (const ClaimUnit& unit : units)
            apply(shared, unit);
        return true;
    }
    return false;
}

void ClaimSharer::share(const JoinedRows::Set& joined)
{
    collectUnits(joined);
    if (m_units.size() < 2 || tryShare(joined, Vector<ClaimUnit, 8>(m_units.span())))
        return;

    // A field that holds nothing but markers fits wherever there are markers: it goes with the kind that most layouts have.
    std::array<size_t, numberOfSharedValueKinds> numberOfRows { };
    for (const ClaimUnit& unit : m_units) {
        if (unit.kind != SharedValueKind::OnlyMarkers)
            numberOfRows[static_cast<unsigned>(unit.kind)] += unit.rows.size();
    }
    auto mostCommonKind = static_cast<SharedValueKind>(std::ranges::max_element(numberOfRows) - numberOfRows.begin());
    for (unsigned kind = 0; kind < numberOfSharedValueKinds; ++kind) {
        Vector<ClaimUnit, 8> units;
        for (const ClaimUnit& unit : m_units) {
            SharedValueKind kindOfUnit = unit.kind == SharedValueKind::OnlyMarkers ? mostCommonKind : unit.kind;
            if (static_cast<unsigned>(kindOfUnit) == kind)
                units.append(unit);
        }
        if (units.size() < m_units.size())
            tryShare(joined, WTF::move(units));
    }
}

void ClaimSharer::run()
{
    computeSpareBytes();
    for (const JoinedRows::Set& joined : m_joinedRows.sets())
        share(joined);
}

Vector<SlotClaim> chooseSlotRepresentations(const SlotPlanContext& context, const SlotUses& uses, const JoinedRows& joinedRows)
{
    Vector<SlotClaim> claims(context.rows.count());
    BitVector fieldsCreatedByStores = findFieldsCreatedByStores(context, uses);
    forEachIndexInParallel(context.build.parallelLoops(), context.program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        if ((context.isCompiledObject(id) && !context.inheritedRows.isInFamily(id)) || context.isEnvironment(id))
            claimRowsOf(context, uses, fieldsCreatedByStores, id, claims.mutableSpan());
    });
    std::span<const InheritedRows::Family> families = context.inheritedRows.families();
    forEachIndexInParallel(context.build.parallelLoops(), families.size(), [&](unsigned index) {
        claimRowsOf(context, uses, fieldsCreatedByStores, families[index], claims.mutableSpan());
    });
    ClaimSharer(context, joinedRows, claims.mutableSpan()).run();
    return claims;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
