/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTFunctionKinds.h"
#include "AOTGlobalBindingPlacement.h"
#include "AOTIR.h"
#include "AOTLayoutData.h"
#include "AOTMethodTable.h"
#include "AOTOperationData.h"
#include "AOTRequestRecords.h"
#include "AOTShapeTest.h"
#include "AOTSiteCachePlacement.h"
#include "AOTSlotPlan.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include "JSExportMacros.h"
#include <compare>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/ScopedLambda.h>

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
struct KeyCellUse;
struct PlacedSites;
struct PlacementContext;
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
    // Some member may list the field without storage, so a proof that the slot is filled does not say that the Extension holds its granule.
    bool mayBeFilledWithoutStorage { false };
    // The members keep the field at different offsets of the cell. The slot has the lowest of them, and a table by shape says how far behind it the slot of an object lies.
    bool findsOffsetByShape { false };
    LayoutSetID membersWithField;
    ShapeTest membersWithFieldTest;
    TypeID contents;
    // The field of every receiver holds the inner object of that receiver, which lies this far behind its start.
    std::optional<uint32_t> offsetOfInnerObject;
};

struct PlacedAllocation {
    LayoutID layout;
    uint32_t shape { 0 };
    FunctionID constructedBy;
    Vector<FieldIndex> initialFields;
    bool holdsInnerObjects { false };
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
    JS_EXPORT_PRIVATE AllocationID find(LayoutID, uint32_t shape, std::span<const FieldIndex> initialFields, bool holdsInnerObjects = false) const;

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

// The fields that one store, or one run of stores, creates in an object of the layout, and the places in the orders of the image where exactly these fields come next, the most likely place first.
struct PlacedFieldCreation {
    static constexpr unsigned maxContinuations = 2;

    LayoutID layout;
    Vector<FieldIndex> fields;
    Vector<KeyOrderContinuation, maxContinuations> continuations;
};

inline constexpr OpcodeSet storesThatMayAdd { Opcode::PutSlotOrAdd, Opcode::PutSlots };
// Compact code asks the run time each time, so it gets no number: the numbers of the places that remember lie close together.
inline bool remembersKeyOrderStep(const Block& block, const Value& value) { return block.shape == CodeShape::Fast && storesThatMayAdd.contains(value.opcode); }

struct KeyOrders {
    std::span<const Vector<FieldIndex>> of(LayoutID layout) const { return layout.index() < ofLayout.size() ? ofLayout[layout.index()].span() : std::span<const Vector<FieldIndex>> { }; }
    JS_EXPORT_PRIVATE std::optional<uint32_t> findOrderWord(LayoutID, std::span<const FieldIndex> fields) const;
    JS_EXPORT_PRIVATE std::span<const KeyOrderContinuation> continuationsCreating(LayoutID, std::span<const FieldIndex> fields) const;
    // The number, among those of the whole program, of a store that may add a field: the one that this many such stores of the function stand in front of, blocks and values in their sequence.
    JS_EXPORT_PRIVATE std::optional<uint32_t> numberOfStoreThatMayAdd(FunctionID, uint32_t storesInFront) const;

    Vector<Vector<Vector<FieldIndex>>> ofLayout;
    Vector<PlacedFieldCreation> fieldCreations;
    Vector<uint32_t> firstStoreThatMayAdd;
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
    // By module. A module has a word only where something may ask for what the word holds: see ModuleData::value.
    Vector<std::optional<uint32_t>> moduleValueOffsets;
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
    bool holdsWhileClear { false };
};

// The bits of classes come first, in the order of their guards.
struct GuardBitTable {
    JS_EXPORT_PRIVATE GuardBitID find(LayoutID, ClassSide, MemberGuardKind, PropertyKeyID) const;
    JS_EXPORT_PRIVATE GuardBitID find(PayloadListView<MemberConditionRecord>) const;
    GuardBitID find(std::span<const struct GuardUse> guardsInOrder) const;
    JS_EXPORT_PRIVATE std::span<const PlacedGuardBit> ofClass(LayoutID) const;
    JS_EXPORT_PRIVATE std::span<const GuardBitID> membersOf(GuardBitID set) const;
    JS_EXPORT_PRIVATE std::span<const GuardBitID> setsContaining(GuardBitID member) const;

    Vector<PlacedGuardBit> bits;
    size_t numberOfClassBits { 0 };
    Vector<uint32_t> firstMemberOfSet;
    Vector<GuardBitID> membersOfSets;
    Vector<uint32_t> firstSetOfClassBit;
    Vector<GuardBitID> setsOfClassBits;
};

bool isAwaitedBeforeClassExists(const Program&, const PlacedGuardBit&);
bool setHoldsWhileClear(const Program&, const GuardBitTable&, std::span<const GuardBitID> members);

struct RequireEdge {
    ModuleID referrer;
    ModuleID target;

    friend auto operator<=>(const RequireEdge&, const RequireEdge&) = default;
};

struct ConstantNumbering {
    Vector<BuiltinCellKey> builtinCells;
    Vector<uint32_t> eagerBuiltinCells;
    Vector<StringID> bigIntConstants;
    // The keys whose cell compiled code names, in the order of their cells.
    Vector<PropertyKeyID> keyCells;
    // Indexed by key: one more than the number of its cell, or zero.
    Vector<uint32_t> keyCellNumbersPlusOne;
    Vector<RequireEdge> requireEdges;
    Vector<StringID> requireEdgeSpecifiers;
};

// Which cell a closure of a function can be.
enum class ClosureIdentity : uint8_t { Unknown, TableCell };

struct FunctionNumbering {
    Vector<FunctionKindData> kinds;
    Vector<LayoutID> constructedClasses;
    Vector<ClosureIdentity> closureIdentities;
    Vector<uint32_t> closureCellNumbers;
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
    bool hasModuleValue(ModuleID id) const { return m_statics.moduleValueOffsets[id.index()].has_value(); }
    uint32_t moduleValueOffset(ModuleID id) const { return m_statics.moduleValueOffsets[id.index()].value(); }
    std::optional<uint32_t> freeCellOffset(LayoutID id) const { return id.index() < m_statics.freeCellOffsets.size() ? m_statics.freeCellOffsets[id.index()] : std::nullopt; }
    JS_EXPORT_PRIVATE std::optional<uint32_t> moduleBuiltinCellOffset(BuiltinID, ModuleID) const;

    const GuardBitTable& guardBits() const { return m_guardBits; }
    const PlacedGuardBit& guardBit(GuardBitID id) const { return id.index() < m_guardBits.bits.size() ? m_guardBits.bits[id.index()] : m_unplaced.guardBit; }

    const MethodTable& methodTable() const { return m_methodTable; }

    const GlobalBindingTable& globalBindings() const { return m_globalBindings; }

    JS_EXPORT_PRIVATE std::optional<uint32_t> builtinCell(BuiltinCellKey) const;
    std::optional<uint32_t> builtinCell(BuiltinID id) const { return builtinCell(BuiltinCellKey::object(id)); }
    std::span<const BuiltinCellKey> builtinCells() const { return m_constants.builtinCells.span(); }
    std::span<const uint32_t> eagerBuiltinCells() const { return m_constants.eagerBuiltinCells.span(); }
    JS_EXPORT_PRIVATE std::optional<uint32_t> bigIntConstant(StringID digits) const;
    std::span<const StringID> bigIntConstants() const { return m_constants.bigIntConstants.span(); }
    std::optional<uint32_t> keyCell(PropertyKeyID key) const
    {
        if (!key || key.index() >= m_constants.keyCellNumbersPlusOne.size() || !m_constants.keyCellNumbersPlusOne[key.index()])
            return std::nullopt;
        return m_constants.keyCellNumbersPlusOne[key.index()] - 1;
    }
    std::span<const PropertyKeyID> keyCells() const { return m_constants.keyCells.span(); }
    JS_EXPORT_PRIVATE std::optional<uint32_t> requireEdge(ModuleID referrer, ModuleID target) const;
    std::span<const RequireEdge> requireEdges() const { return m_constants.requireEdges.span(); }
    std::span<const StringID> requireEdgeSpecifiers() const { return m_constants.requireEdgeSpecifiers.span(); }

    FunctionKindData functionKind(FunctionID id) const { return id.index() < m_functions.kinds.size() ? m_functions.kinds[id.index()] : FunctionKindData { }; }
    LayoutID classConstructedBy(FunctionID id) const { return id.index() < m_functions.constructedClasses.size() ? m_functions.constructedClasses[id.index()] : LayoutID(); }
    // How many layouts with a smaller index have a class record: the index of the class's word in ClassWords::classPrototypes.
    // False for a record or a class that no body names any more: the tables do not describe it.
    bool isInUse(LayoutID id) const { return !m_layoutsOutOfUse.get(id.index()); }
    std::optional<uint32_t> rankOfClass(LayoutID id) const { return id.index() < m_classRanks.size() ? m_classRanks[id.index()] : std::nullopt; }
    ClosureIdentity closureIdentityOf(FunctionID id) const { return id.index() < m_functions.closureIdentities.size() ? m_functions.closureIdentities[id.index()] : ClosureIdentity::Unknown; }
    // 0 means the function has no cell in the closure table.
    uint32_t closureCellNumber(FunctionID id) const { return id.index() < m_functions.closureCellNumbers.size() ? m_functions.closureCellNumbers[id.index()] : 0; }
    ClosureFunctionRange closureFunctions() const { return m_functions.closureFunctions; }

    const RequestTable& requests() const { return m_requests; }
    const PlacedPropertyRuns& propertyRuns() const { return m_propertyRuns; }
    const BitVector& typesNeededAtRunTime() const { return m_typesNeededAtRunTime; }
    // Those, and the types that code tests.
    const BitVector& typesWithCells() const { return m_typesWithCells; }
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
    friend class PlacementInSteps;

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
    MethodTable m_methodTable;
    GlobalBindingTable m_globalBindings;
    ConstantNumbering m_constants;
    FunctionNumbering m_functions;
    Vector<std::optional<uint32_t>> m_classRanks;
    BitVector m_layoutsOutOfUse;
    RequestTable m_requests;
    PlacedPropertyRuns m_propertyRuns;
    BitVector m_typesNeededAtRunTime;
    BitVector m_typesWithCells;
    BitVector m_stringConstantsInTables;
    unsigned m_numberOfRegExpRecords { 0 };
    UnplacedRecords m_unplaced;
};

// What numberAndEncode() takes from one selected body by itself, for whoever has the body in the cache. Of the inputs it reads flow, heat and the slot plan.
class BodyScansForPlacement {
    WTF_MAKE_NONCOPYABLE(BodyScansForPlacement);
public:
    BodyScansForPlacement(const Program&, const PlacementInputs&, BuildContext&);
    ~BodyScansForPlacement();

    // From any thread, no two for one function at a time. A later scan of a function replaces an earlier one.
    void scan(FunctionID);
    // By one thread, while nobody scans: the program has got functions.
    void didAddFunctions();
    // In the functions that had a body when the sites were taken: no more site caches are asked for than that.
    uint64_t numberOfOperationsWithWordForSiteCache() const;

    // For numberAndEncode().
    const Vector<KeyCellUse>& keyCellUsesOf(FunctionID) const LIFETIME_BOUND;
    const Vector<GlobalBindingSite>& globalBindingSitesOf(FunctionID) const LIFETIME_BOUND;
    PlacedSites takeSites(const PlacementContext&);
    void destroyLater();

private:
    struct State;
    std::unique_ptr<State> m_state;
};

// numberAndEncode() in three steps, for a caller that has representations chosen beside the second.
class PlacementInSteps {
    WTF_MAKE_NONCOPYABLE(PlacementInSteps);
public:
    // With scans: every function that has a body was scanned as it is now.
    PlacementInSteps(const PlacementInputs&, BuildContext&, BodyScansForPlacement*);
    ~PlacementInSteps();

    // All of numbering that reads a body, and the words of global bindings.
    Placement numberWhatRepresentationsAsk(ProgramEditor&, NOESCAPE const ScopedLambda<void()>& readBodiesBesideCollectingSites);
    // Reads no body and changes nothing of the program. It leaves where it is what the first step has answered, so others may go on asking for that.
    void numberRest(Placement&);
    // The words of sites. All of them, or those that are left: see resolveFieldAccessesOf().
    void resolveSites(ProgramEditor&, Placement&);

    // Between the first step and the third, for a caller that changes the bodies in between: see resolveFieldAccesses() in AOTPlacementSteps.h.
    void prepareToResolveFieldAccesses();
    // Once for each function with a body, from any thread.
    void resolveFieldAccessesOf(const Placement&, FunctionID, Function&);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

// The table of modules keeps the path of a file in pieces: see ModuleDirectoryData. The pieces have to be strings of the program before its strings are numbered.
JS_EXPORT_PRIVATE void internPiecesOfModulePaths(Program&);
JS_EXPORT_PRIVATE Placement numberAndEncode(ProgramEditor&, const PlacementInputs&, BuildContext&);
// With scans: every function that has a body was scanned as it is now.
Placement numberAndEncode(ProgramEditor&, const PlacementInputs&, BuildContext&, NOESCAPE const ScopedLambda<void()>& readBodiesBesideCollectingSites, BodyScansForPlacement* = nullptr);
JS_EXPORT_PRIVATE void checkPlacement(const Program&, const PlacementInputs&, const Placement&, const SiteCaches&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
