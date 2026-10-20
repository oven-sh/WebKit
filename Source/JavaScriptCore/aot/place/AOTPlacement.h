/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTFunctionKinds.h"
#include "AOTIR.h"
#include "AOTLayoutData.h"
#include "AOTOperationData.h"
#include "AOTRequestRecords.h"
#include "AOTShapeTest.h"
#include "AOTSiteCachePlacement.h"
#include "AOTSlotPlan.h"
#include "AOTVerifier.h"
#include "JSExportMacros.h"
#include <compare>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;
class RegExpRequests;

namespace Typed {

class CallGraph;
class CallingConventions;
class ConstantCollections;
class EnvironmentReuse;
class FlowResults;
class FrameAllocation;
class Heat;
class LiteralStringTable;
struct PlacementParts;
struct ResolvedSites;

struct FieldAccess {
    enum class Coverage : uint8_t {
        NoMember,
        SomeMembers,
        EveryMemberHasSlot,
        EveryMember,
    };

    PropertyKeyID key;
    LayoutSetID receivers;
    bool isStore { false };
    Coverage coverage { Coverage::NoMember };
    PlacedSlot slot;
    PlacedSlot extensionWord;
    LayoutSetID membersWithField;
    ShapeTest membersWithFieldTest;
    TypeID contents;
};

struct PlacedAllocation {
    LayoutID layout;
    uint32_t shape { 0 };
    FunctionID constructedBy;
    Vector<FieldIndex> initialFields;
    uint16_t numberOfCaptures { 0 };
    uint32_t size { 0 };
    uint32_t orderWord { 0 };
};

struct AllocationTable {
    std::span<const AllocationID> ofLayout(LayoutID layout) const
    {
        if (static_cast<size_t>(layout.index()) + 1 >= firstOfLayout.size())
            return { };
        uint32_t first = firstOfLayout[layout.index()];
        return byLayout.subspan(first, firstOfLayout[layout.index() + 1] - first);
    }
    JS_EXPORT_PRIVATE AllocationID find(LayoutID, uint32_t shape, std::span<const FieldIndex> initialFields) const;

    Vector<PlacedAllocation> allocations;
    Vector<AllocationID> closureAllocations;
    Vector<uint32_t> firstOfLayout;
    Vector<AllocationID> byLayout;
};

// The order word of an object whose keys are the part of an order before some fields, and its order word once those fields are created.
struct KeyOrderContinuation {
    uint32_t orderWordBefore { 0 };
    uint32_t orderWordAfter { 0 };

    friend bool operator==(const KeyOrderContinuation&, const KeyOrderContinuation&) = default;
};

// The fields that one store, or one run of stores, creates in an object of the layout, and the places in the orders of the image where exactly
// these fields come next, the most likely place first.
struct PlacedFieldCreation {
    static constexpr unsigned maxContinuations = 2;

    LayoutID layout;
    Vector<FieldIndex> fields;
    Vector<KeyOrderContinuation, maxContinuations> continuations;
};

struct KeyOrders {
    std::span<const Vector<FieldIndex>> of(LayoutID layout) const { return layout.index() < ofLayout.size() ? ofLayout[layout.index()].span() : std::span<const Vector<FieldIndex>> { }; }
    JS_EXPORT_PRIVATE std::optional<uint32_t> findOrderWord(LayoutID, std::span<const FieldIndex> fields) const;
    JS_EXPORT_PRIVATE std::span<const KeyOrderContinuation> continuationsCreating(LayoutID, std::span<const FieldIndex> fields) const;

    Vector<Vector<Vector<FieldIndex>>> ofLayout;
    Vector<PlacedFieldCreation> fieldCreations;
};

enum class StaticsRegion : uint8_t {
    FilledAtInstanceCreation,
    FilledAtProgramEntry,
};

struct PlacedVariable {
    bool hasWordInStatics() const { return storage == VariableStorage::Statics; }

    VariableStorage storage { VariableStorage::None };
    VariableEncoding encoding { VariableEncoding::Plain };
    SlotRepresentation representation { SlotRepresentation::JSValue };
    StaticsRegion region { StaticsRegion::FilledAtInstanceCreation };
    uint32_t offset { 0 };
    uint64_t initialWord { 0 };
    FunctionID function;
};

struct ModuleBuiltinCell {
    BuiltinID builtin;
    ModuleID module;
    uint32_t offset { 0 };
};

struct StaticsPlan {
    Vector<PlacedVariable> variables;
    Vector<uint32_t> moduleValueOffsets;
    Vector<std::optional<uint32_t>> freeCellOffsets;
    Vector<ModuleBuiltinCell> moduleBuiltinCells;
    Vector<uint32_t> roots;
    Vector<StaticsRange> clearedEachCollection;
    uint32_t distanceFromArrayProfileToLastArray { 0 };
    uint32_t sizeFilledAtProgramEntry { 0 };
    uint32_t size { 0 };
};

struct PlacedGuardBit {
    uint64_t mask() const { return 1ull << bit % MemberGuardData::bitsPerWord; }

    LayoutID classLayout;
    ClassSide side { ClassSide::Prototype };
    MemberGuardKind kind { MemberGuardKind::Member };
    PropertyKeyID key;
    uint32_t bit { 0 };
    uint32_t wordOffset { 0 };
};

struct GuardBitTable {
    JS_EXPORT_PRIVATE GuardBitID find(LayoutID, ClassSide, MemberGuardKind, PropertyKeyID) const;
    JS_EXPORT_PRIVATE std::span<const PlacedGuardBit> ofClass(LayoutID) const;

    Vector<PlacedGuardBit> bits;
};

struct RequireEdge {
    ModuleID referrer;
    ModuleID target;

    friend auto operator<=>(const RequireEdge&, const RequireEdge&) = default;
};

struct ConstantNumbering {
    Vector<BuiltinCellKey> builtinCells;
    Vector<uint32_t> eagerBuiltinCells;
    Vector<StringID> bigIntConstants;
    Vector<RequireEdge> requireEdges;
    Vector<StringID> requireEdgeSpecifiers;
};

// Which cell a closure of a function can be.
enum class ClosureIdentity : uint8_t { Unknown, TableCell };

struct FunctionNumbering {
    Vector<FunctionKindData> kinds;
    Vector<LayoutID> constructedClasses;
    Vector<ClosureIdentity> closureIdentities;
    ClosureFunctionRange closureFunctions;
};

struct PlacementInputs {
    const FlowResults& flow;
    const CallGraph& callGraph;
    const Heat& heat;
    const CallingConventions& callingConventions;
    const FrameAllocation& frameAllocation;
    const EnvironmentReuse& environmentReuse;
    const LiteralStringTable& literalStrings;
    const ConstantCollections& constantCollections;
    const SlotPlan& slots;
    const RegExpRequests& regExpRequests;
};

class Placement {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Placement);
public:
    Placement() = default;
    JS_EXPORT_PRIVATE explicit Placement(PlacementParts&&);
    void setResolvedSites(ResolvedSites&&);

    const SlotPlan& slots() const { return m_slots ? *m_slots : m_unplaced.slots; }
    const PlannedLayout& layout(LayoutID id) const { return slots().layout(id); }
    const PlacedSlot& slot(LayoutID id, FieldIndex field) const { return slots().slot(id, field); }

    const ShapeTable& shapes() const { return m_shapes; }

    const FieldAccess& fieldAccess(FieldAccessID id) const { return id.index() < m_fieldAccesses.size() ? m_fieldAccesses[id.index()] : m_unplaced.fieldAccess; }
    JS_EXPORT_PRIVATE FieldAccessID findFieldAccess(PropertyKeyID, LayoutSetID receivers, bool isStore) const;
    unsigned numberOfFieldAccesses() const { return m_fieldAccesses.size(); }

    const AllocationTable& allocations() const { return m_allocations; }
    const PlacedAllocation& allocation(AllocationID id) const { return id.index() < m_allocations.allocations.size() ? m_allocations.allocations[id.index()] : m_unplaced.allocation; }
    AllocationID closureAllocation(unsigned numberOfCaptures) const { return numberOfCaptures / 2 < m_allocations.closureAllocations.size() ? m_allocations.closureAllocations[numberOfCaptures / 2] : AllocationID(); }
    LayoutID closureLayout() const { return m_closureLayout; }
    unsigned numberOfAllocations() const { return m_allocations.allocations.size(); }

    const KeyOrders& keyOrders() const { return m_keyOrders; }

    const StaticsPlan& statics() const { return m_statics; }
    const PlacedVariable& variable(VariableID id) const { return id.index() < m_statics.variables.size() ? m_statics.variables[id.index()] : m_unplaced.variable; }
    uint32_t sizeOfStatics() const { return m_statics.size; }
    uint32_t moduleValueOffset(ModuleID id) const { return m_statics.moduleValueOffsets[id.index()]; }
    std::optional<uint32_t> freeCellOffset(LayoutID id) const { return id.index() < m_statics.freeCellOffsets.size() ? m_statics.freeCellOffsets[id.index()] : std::nullopt; }
    JS_EXPORT_PRIVATE std::optional<uint32_t> moduleBuiltinCellOffset(BuiltinID, ModuleID) const;

    const GuardBitTable& guardBits() const { return m_guardBits; }
    const PlacedGuardBit& guardBit(GuardBitID id) const { return id.index() < m_guardBits.bits.size() ? m_guardBits.bits[id.index()] : m_unplaced.guardBit; }

    JS_EXPORT_PRIVATE std::optional<uint32_t> builtinCell(BuiltinCellKey) const;
    std::optional<uint32_t> builtinCell(BuiltinID id) const { return builtinCell(BuiltinCellKey::object(id)); }
    std::span<const BuiltinCellKey> builtinCells() const { return m_constants.builtinCells.span(); }
    std::span<const uint32_t> eagerBuiltinCells() const { return m_constants.eagerBuiltinCells.span(); }
    JS_EXPORT_PRIVATE std::optional<uint32_t> bigIntConstant(StringID digits) const;
    std::span<const StringID> bigIntConstants() const { return m_constants.bigIntConstants.span(); }
    JS_EXPORT_PRIVATE std::optional<uint32_t> requireEdge(ModuleID referrer, ModuleID target) const;
    std::span<const RequireEdge> requireEdges() const { return m_constants.requireEdges.span(); }
    std::span<const StringID> requireEdgeSpecifiers() const { return m_constants.requireEdgeSpecifiers.span(); }

    FunctionKindData functionKind(FunctionID id) const { return id.index() < m_functions.kinds.size() ? m_functions.kinds[id.index()] : FunctionKindData { }; }
    LayoutID classConstructedBy(FunctionID id) const { return id.index() < m_functions.constructedClasses.size() ? m_functions.constructedClasses[id.index()] : LayoutID(); }
    ClosureIdentity closureIdentityOf(FunctionID id) const { return id.index() < m_functions.closureIdentities.size() ? m_functions.closureIdentities[id.index()] : ClosureIdentity::Unknown; }
    ClosureFunctionRange closureFunctions() const { return m_functions.closureFunctions; }

    const RequestTable& requests() const { return m_requests; }
    const PlacedPropertyRuns& propertyRuns() const { return m_propertyRuns; }
    const BitVector& typesNeededAtRunTime() const { return m_typesNeededAtRunTime; }
    const BitVector& stringConstantsInTables() const { return m_stringConstantsInTables; }

    PlacedTableSizes tableSizes() const
    {
        return {
            .fieldAccesses = numberOfFieldAccesses(),
            .allocations = numberOfAllocations(),
            .regExpRecords = m_numberOfRegExpRecords,
            .guardBits = static_cast<unsigned>(m_guardBits.bits.size()),
        };
    }

    JS_EXPORT_PRIVATE void dump(PrintStream&, const Program&) const;

private:
    struct UnplacedRecords {
        SlotPlan slots;
        FieldAccess fieldAccess;
        PlacedAllocation allocation;
        PlacedVariable variable;
        PlacedGuardBit guardBit;
    };

    const SlotPlan* m_slots { nullptr };
    ShapeTable m_shapes;
    Vector<FieldAccess> m_fieldAccesses;
    AllocationTable m_allocations;
    LayoutID m_closureLayout;
    KeyOrders m_keyOrders;
    StaticsPlan m_statics;
    GuardBitTable m_guardBits;
    ConstantNumbering m_constants;
    FunctionNumbering m_functions;
    RequestTable m_requests;
    PlacedPropertyRuns m_propertyRuns;
    BitVector m_typesNeededAtRunTime;
    BitVector m_stringConstantsInTables;
    unsigned m_numberOfRegExpRecords { 0 };
    UnplacedRecords m_unplaced;
};

JS_EXPORT_PRIVATE Placement numberAndEncode(ProgramEditor&, const PlacementInputs&, BuildContext&);
JS_EXPORT_PRIVATE void checkPlacement(const Program&, const PlacementInputs&, const Placement&, const SiteCaches&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
