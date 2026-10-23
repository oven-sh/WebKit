/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellSizeClasses.h"
#include "AOTGlobalBindingPlacement.h"
#include "AOTPlacement.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTSiteWeight.h"
#include "AOTSlotPlanSteps.h"
#include "AOTVector.h"
#include <algorithm>
#include <compare>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

struct PlacementContext {
    const SlotPlan& slots() const { return inputs.slots; }
    bool isCompiledObject(LayoutID id) const { return inputs.slots.storageOf(id) == LayoutStorage::CompiledObject; }
    bool isEnvironment(LayoutID id) const { return inputs.slots.storageOf(id) == LayoutStorage::Environment; }
    bool receivesShape(LayoutID id) const { return isCompiledObject(id) || isEnvironment(id) || id == closureLayout; }

    const Program& program;
    const PlacementInputs& inputs;
    const CellSizeClasses& sizeClasses;
    BuildContext& build;
    bool usesWideReferences;
    LayoutID closureLayout;
};

struct SiteKey {
    FunctionID function;
    ValueID value;

    friend auto operator<=>(const SiteKey&, const SiteKey&) = default;
};

struct SourceSite {
    FunctionID function;
    uint32_t position { 0 };
    StringID pattern;
    uint32_t regExpFlags { 0 };

    friend auto operator<=>(const SourceSite&, const SourceSite&) = default;
};

SourceSite sourceSiteOf(const Program&, FunctionID, const Value&);

struct FieldAccessUse {
    SlotAccessKey access;
    // Right where some receiver has no slot for the field, so that the access tests shapes. Nobody reads another's, and it may lack sites.
    SiteWeight weight { 0 };
    uint32_t sites { 0 };
};

struct ShapeTestUse {
    LayoutSetID tested;
    LayoutSetID known;
    SiteWeight weight { 0 };
    uint32_t sites { 0 };
};

struct AllocationUse {
    LayoutID layout;
    bool keysAreNeverEnumerated { false };
    bool holdsInnerObjects { false };
    Vector<FieldIndex> initialFields;

    friend bool operator==(const AllocationUse&, const AllocationUse&) = default;
    friend bool operator<(const AllocationUse& a, const AllocationUse& b)
    {
        if (a.layout != b.layout)
            return a.layout < b.layout;
        if (a.keysAreNeverEnumerated != b.keysAreNeverEnumerated)
            return b.keysAreNeverEnumerated;
        if (a.holdsInnerObjects != b.holdsInnerObjects)
            return b.holdsInnerObjects;
        return std::ranges::lexicographical_compare(a.initialFields, b.initialFields);
    }
};

struct KeyOrderTestUse {
    LayoutID layout;
    Vector<FieldIndex> fields;

    friend bool operator==(const KeyOrderTestUse&, const KeyOrderTestUse&) = default;
    friend bool operator<(const KeyOrderTestUse& a, const KeyOrderTestUse& b)
    {
        if (a.layout != b.layout)
            return a.layout < b.layout;
        return std::ranges::lexicographical_compare(a.fields, b.fields);
    }
};

// The fields that one store, or one run of stores, may create in an object of the layout, in the sequence of the stores.
struct FieldCreationUse {
    LayoutID layout;
    Vector<FieldIndex> fields;

    friend bool operator==(const FieldCreationUse&, const FieldCreationUse&) = default;
    friend bool operator<(const FieldCreationUse& a, const FieldCreationUse& b)
    {
        if (a.layout != b.layout)
            return a.layout < b.layout;
        return std::ranges::lexicographical_compare(a.fields, b.fields);
    }
};

struct GuardUse {
    LayoutID classLayout;
    ClassSide side { ClassSide::Prototype };
    MemberGuardKind kind { MemberGuardKind::Member };
    PropertyKeyID key;
    SiteWeight weight { 0 };
};

// A call through the method table: the pairs of a shape and a selector that it can ask for.
struct TableCallUse {
    friend auto operator<=>(const TableCallUse&, const TableCallUse&) = default;

    MethodSelector selector;
    LayoutSetID receivers;
};

// The guards of a site with several conditions: in the order of their keys, each once, two or more.
struct GuardSetUse {
    Vector<GuardUse, 4> guards;
    SiteWeight weight { 0 };
};

struct ModuleBuiltinUse {
    BuiltinID builtin;
    ModuleID module;

    friend auto operator<=>(const ModuleBuiltinUse&, const ModuleBuiltinUse&) = default;
};

struct PlacedSites {
    Vector<FieldAccessUse> fieldAccesses;
    Vector<ShapeTestUse> shapeTests;
    Vector<AllocationUse> allocations;
    Vector<KeyOrderTestUse> keyOrderTests;
    Vector<FieldCreationUse> fieldCreations;
    // By function: how many stores that may add a field the functions in front of it have. One entry more than there are functions.
    Vector<uint32_t> firstStoreThatMayAdd;
    Vector<GuardUse> guards;
    Vector<GuardSetUse> guardSets;
    Vector<TableCallUse> tableCalls;
    Vector<SourceSite> profiledArrayLiterals;
    Vector<SourceSite> regExpLiterals;
    Vector<SourceSite> templateObjects;
    Vector<DataID> copyOnWriteArrays;
    Vector<DataID> arraysWithStorageInImage;
    Vector<BuiltinCellKey> builtinCells;
    Vector<StringID> bigIntConstants;
    Vector<RequireEdge> requireEdges;
    Vector<StringID> requireEdgeSpecifiers;
    Vector<ModuleBuiltinUse> moduleBuiltins;
    BitVector variablesNamedByCode;
    // Of a type that code hands to the run time the tables hold a record. A type that code only tests needs the cells and the names that its test compares with.
    BitVector typesInRunTimePayloads;
    BitVector typesTestedByCode;
    BitVector functionsWithAllocatedClosures;
    BitVector layoutsOutOfUse;
    unsigned maxClosureCaptures { 0 };
};

struct ShapeVariants {
    bool hasOrderedShape(LayoutID id) const { return ordered.get(id.index()); }
    bool hasUnorderedShape(LayoutID id) const { return unordered.get(id.index()); }
    uint32_t numberOfShapesOf(LayoutID id) const { return hasOrderedShape(id) + hasUnorderedShape(id); }

    BitVector ordered;
    BitVector unordered;
};

struct TestedLayoutList {
    LayoutSetID set;
    uint32_t firstLayout { 0 };
    uint32_t numberOfLayouts { 0 };
    uint32_t numberOfShapes { 0 };
    uint32_t sites { 0 };
    SiteWeight weight { 0 };
};

struct ShapeTestCandidates {
    std::span<const uint32_t> layoutsOf(const TestedLayoutList& list) const { return listedLayouts.subspan(list.firstLayout, list.numberOfLayouts); }

    Vector<TestedLayoutList> lists;
    Vector<uint32_t> listedLayouts;
    Vector<uint32_t> listsByWeight;
    Vector<SiteWeight> singleLayoutTestWeight;
};

struct ShapeNumbering {
    Vector<ShapeRange> shapesOfLayout;
    uint32_t numberOfShapes { 0 };
    uint32_t firstEnvironmentShape { 0 };
};

struct AssignedShapes {
    uint32_t countInRange(uint32_t first, uint32_t last) const { return first > last ? 0 : countPrefixSums[last] - countPrefixSums[first - 1]; }

    std::span<const uint32_t> countPrefixSums;
    uint32_t numberOfShapes { 0 };
};

struct LiteralSites {
    Vector<uint32_t> profiledLiteralOffsets;
    Vector<uint32_t> regExpCellOffsets;
    Vector<uint32_t> templateObjectOffsets;
    Vector<uint32_t> sharedStorageOffsets;
    Vector<uint32_t> nodesOfStorageInImage;
};

struct PlacedOrderWords {
    Vector<uint32_t> ofAllocation;
    Vector<std::optional<uint32_t>> ofTest;
};

struct ResolvedSites {
    RequestTable requests;
    PlacedPropertyRuns propertyRuns;
    BitVector stringConstantsInTables;
};

struct PlacementParts {
    const SlotPlan* slots { nullptr };
    ShapeTable shapes;
    Vector<FieldAccess> fieldAccesses;
    AllocationTable allocations;
    LayoutID closureLayout;
    KeyOrders keyOrders;
    StaticsPlan statics;
    GuardBitTable guardBits;
    MethodTable methodTable;
    GlobalBindingTable globalBindings;
    ConstantNumbering constants;
    FunctionNumbering functions;
    Vector<std::optional<uint32_t>> classRanks;
    BitVector layoutsOutOfUse;
    BitVector typesNeededAtRunTime;
    BitVector typesWithCells;
    unsigned numberOfRegExpRecords { 0 };
};

// Where the program's text names the cell of a key: module, function as written. A cell that no operation names comes behind all that one does.
struct KeyCellUse {
    PropertyKeyID key;
    uint32_t module { UINT32_MAX };
    uint32_t sourceFunction { UINT32_MAX };

    friend auto operator<=>(const KeyCellUse&, const KeyCellUse&) = default;
};

struct GlobalBindingSite {
    GlobalBindingKey key;
    FunctionID function;
    ProgramPoint point;
    ValueID value;
    bool isNamedStore { false };
};

void collectKeyCellUses(const Program&, FunctionID, Vector<KeyCellUse>&);
void collectGlobalBindingSites(const Program&, FunctionID, Vector<GlobalBindingSite>&);

PlacedSites collectPlacedSites(const PlacementContext&);

ShapeVariants chooseShapeVariants(const PlacementContext&, const PlacedSites&);
Vector<FieldAccess> createFieldAccesses(const PlacementContext&, const PlacedSites&, TypeArena&);
ShapeTestCandidates collectShapeTestCandidates(const PlacementContext&, const PlacedSites&, std::span<const FieldAccess>, const ShapeVariants&, const BitVector& typesNeededAtRunTime);
ShapeNumbering numberShapes(const PlacementContext&, const ShapeTestCandidates&, const ShapeVariants&);
ShapeTest chooseShapeTestForm(std::span<const uint32_t> ascendingShapes, const AssignedShapes&);
ShapeTest chooseShapeTestForm(std::span<const uint32_t> ascendingShapes, std::span<const uint32_t> ascendingKnownShapes, const AssignedShapes&);
ShapeTable deriveShapeTests(const PlacementContext&, const PlacedSites&, const ShapeNumbering&, const ShapeVariants&, const BitVector& typesNeededAtRunTime, Vector<FieldAccess>&);

AllocationTable numberAllocations(const PlacementContext&, const PlacedSites&);
std::pair<KeyOrders, PlacedOrderWords> collectKeyOrders(const PlacementContext&, const PlacedSites&, const AllocationTable&);
void finishAllocations(const PlacementContext&, const ShapeTable&, const PlacedOrderWords&, std::span<const AllocationUse>, AllocationTable&);

Vector<GuardUse, 4> guardsOf(PayloadListView<MemberConditionRecord>);
Vector<GuardUse, 4> guardsOf(std::span<const MemberCondition>);
// The functions that the method table names. To find them is to read the bodies of the table's entrypoints as selection has left them.
struct MethodTableFunctions {
    struct EntrypointOfMethod {
        friend auto operator<=>(const EntrypointOfMethod&, const EntrypointOfMethod&) = default;

        FunctionID method;
        MethodSelector selector;
        FunctionID entrypoint;
    };

    struct LookupOfSelector {
        friend auto operator<=>(const LookupOfSelector&, const LookupOfSelector&) = default;

        MethodSelector selector;
        FunctionID lookup;
    };

    FunctionID entrypointOf(FunctionID method, MethodSelector) const;
    FunctionID lookupOf(MethodSelector) const;

    Vector<EntrypointOfMethod> entrypoints;
    Vector<LookupOfSelector> lookups;
};
MethodTableFunctions collectMethodTableFunctions(const PlacementContext&);
MethodTable placeMethodTable(const PlacementContext&, const PlacedSites&, const ShapeTable&, const GuardBitTable&, const MethodTableFunctions&);
GuardBitTable placeGuardBits(const PlacementContext&, const PlacedSites&);
void placeGuardSets(const PlacementContext&, const PlacedSites&, GuardBitTable&);
std::pair<StaticsPlan, LiteralSites> placeStatics(const PlacementContext&, const PlacedSites&, GuardBitTable&);
uint64_t initialWordOf(const Program&, VariableID, const PlannedVariable&);

// What collectKeyCellUses() gives for a function with a body.
using KeyCellUsesOfBodies = ScopedLambda<const Vector<KeyCellUse>&(FunctionID)>;
ConstantNumbering numberConstants(const PlacementContext&, PlacedSites&, const BitVector& typesWithCells, const KeyCellUsesOfBodies* = nullptr);
FunctionNumbering numberFunctions(const PlacementContext&, const PlacedSites&);
enum class TypesTestedByCode : uint8_t { Exclude, Include };
enum class LayoutsOutOfUse : uint8_t { Exclude, Include };
BitVector computeTypesNeededAtRunTime(const PlacementContext&, const PlacedSites&, TypesTestedByCode, LayoutsOutOfUse = LayoutsOutOfUse::Exclude);

struct SiteResolutionInputs {
    const PlacedSites& sites;
    const LiteralSites& literals;
    const PlacedOrderWords& orderWords;
    const Placement& placement;
};

ResolvedSites resolveSites(ProgramEditor&, const PlacementContext&, const SiteResolutionInputs&);
// The same in two parts, for a caller that has the bodies changed between them by somebody who asks for field accesses, adds values and blocks, and leaves every value its number.
void resolveFieldAccesses(const PlacementContext&, const SiteResolutionInputs&, FunctionID, Function&, Vector<ValueID>& valuesWithOtherWords);
// By function.
ResolvedSites resolveOtherWords(ProgramEditor&, const PlacementContext&, const SiteResolutionInputs&, std::span<const Vector<ValueID>> valuesWithOtherWords);
// The request of such an operation is the number of a run of property keys: the keys it stores under, or the keys a copy leaves out.
bool namesPropertyRun(Opcode);
std::span<const PropertyKeyID> keysOfPropertyRun(const Value&);
std::optional<PlacedPropertyRun> propertyRunOf(const PlacementContext&, FunctionID, ValueID);
// Whether propertyRunOf() gives one. It reports nothing.
bool hasPropertyRun(const Program&, const BuildContext&, const Value&);
void collectStringConstantsOf(const Program&, const RequestRecord&, BitVector& stringConstants);
void collectStringConstantsOf(const Program&, const DataNode&, BitVector& stringConstants);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
