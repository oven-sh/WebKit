/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellSizeClasses.h"
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

inline bool hasArrayProfile(ArrayLiteralStorage storage)
{
    switch (storage) {
    case ArrayLiteralStorage::Fresh:
        return false;
    case ArrayLiteralStorage::Profiled:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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
    Vector<FieldIndex> initialFields;

    friend bool operator==(const AllocationUse&, const AllocationUse&) = default;
    friend bool operator<(const AllocationUse& a, const AllocationUse& b)
    {
        if (a.layout != b.layout)
            return a.layout < b.layout;
        if (a.keysAreNeverEnumerated != b.keysAreNeverEnumerated)
            return b.keysAreNeverEnumerated;
        return std::ranges::lexicographical_compare(a.initialFields, b.initialFields);
    }
};

struct FieldsOfLayout {
    LayoutID layout;
    Vector<FieldIndex> fields;

    friend bool operator==(const FieldsOfLayout&, const FieldsOfLayout&) = default;
    friend bool operator<(const FieldsOfLayout& a, const FieldsOfLayout& b)
    {
        if (a.layout != b.layout)
            return a.layout < b.layout;
        return std::ranges::lexicographical_compare(a.fields, b.fields);
    }
};

using KeyOrderTestUse = FieldsOfLayout;
// The fields that one store, or one run of stores, may create in an object of the layout, in the sequence of the stores.
using FieldCreationUse = FieldsOfLayout;

struct GuardUse {
    LayoutID classLayout;
    ClassSide side { ClassSide::Prototype };
    MemberGuardKind kind { MemberGuardKind::Member };
    PropertyKeyID key;
    SiteWeight weight { 0 };
};

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

struct RequireEdgeUse {
    RequireEdge edge;
    StringID specifier;

    friend auto operator<=>(const RequireEdgeUse&, const RequireEdgeUse&) = default;
};

struct ModuleBuiltinUse {
    BuiltinID builtin;
    ModuleID module;

    friend auto operator<=>(const ModuleBuiltinUse&, const ModuleBuiltinUse&) = default;
};

// Where the program's text names the cell of a key: module, function as written. A cell that no operation names comes behind all that one does.
struct KeyCellUse {
    PropertyKeyID key;
    uint32_t module { UINT32_MAX };
    uint32_t sourceFunction { UINT32_MAX };

    friend auto operator<=>(const KeyCellUse&, const KeyCellUse&) = default;
};

// Of one body, or of all of them: then ascending, each once.
struct SiteLists {
    Vector<FieldAccessUse> fieldAccesses;
    Vector<ShapeTestUse> shapeTests;
    Vector<AllocationUse> allocations;
    Vector<KeyOrderTestUse> keyOrderTests;
    Vector<FieldCreationUse> fieldCreations;
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
    Vector<RequireEdgeUse> requireEdges;
    Vector<ModuleBuiltinUse> moduleBuiltins;
    unsigned maxClosureCaptures { 0 };
};

struct PlacedSites : SiteLists {
    // The fields that stores may create, per layout, in the order in which the bodies contain the stores.
    Vector<FieldCreationUse> sourceFieldCreations;
    // By function: how many stores that may add a field the functions in front of it have. One entry more than there are functions.
    Vector<uint32_t> firstStoreThatMayAdd;
    // By store that may add a field, the functions in their sequence.
    Vector<PlaceOfStoreThatMayAdd> placesOfStoresThatMayAdd;
    BitVector variablesNamedByCode;
    // Of a type that code hands to the run time the tables hold a record. A type that code only tests needs the cells and the names that its test compares with.
    BitVector typesInRunTimePayloads;
    BitVector typesTestedByCode;
    BitVector functionsWithAllocatedClosures;
    BitVector layoutsOutOfUse;
    BitVector bodiesWithoutCode;
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

struct GlobalBindingSite {
    GlobalBindingKey key;
    FunctionID function;
    ProgramPoint point;
    ValueID value;
    bool isNamedStore { false };
};

template<typename Site>
Vector<std::span<const Site>> groupSitesByFunction(std::span<const Site> sites)
{
    Vector<std::span<const Site>> groups;
    size_t first = 0;
    for (size_t index = 1; index <= sites.size(); ++index) {
        if (index < sites.size() && sites[index].function == sites[first].function)
            continue;
        groups.append(sites.subspan(first, index - first));
        first = index;
    }
    return groups;
}

LayoutID findClosureLayout(const Program&);
GlobalBindingTable placeGlobalBindings(ProgramEditor&, const PlacementContext&, TypeArena&, BodyScansForPlacement&);

ShapeVariants chooseShapeVariants(const PlacementContext&, const PlacedSites&);
ShapeTestCandidates collectShapeTestCandidates(const PlacementContext&, const PlacedSites&, std::span<const FieldAccess>, const ShapeVariants&, const BitVector& typesNeededAtRunTime);
ShapeNumbering numberShapes(const PlacementContext&, const ShapeTestCandidates&, const ShapeVariants&);
ShapeTest chooseShapeTestForm(std::span<const uint32_t> ascendingShapes, const AssignedShapes&);
ShapeTable deriveShapeTests(const PlacementContext&, const PlacedSites&, const ShapeNumbering&, const ShapeVariants&, const BitVector& typesNeededAtRunTime, Vector<FieldAccess>&);

FunctionNumbering numberFunctions(const PlacementContext&, const PlacedSites&);
std::pair<KeyOrders, PlacedOrderWords> collectKeyOrders(const PlacementContext&, const PlacedSites&, const AllocationTable&);

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

struct SiteResolutionInputs {
    const PlacedSites& sites;
    const LiteralSites& literals;
    const PlacedOrderWords& orderWords;
    const Placement& placement;
};

// The words of sites in two parts. Between them the bodies may be changed by somebody who asks for field accesses, adds values and blocks, and leaves every value its number.
void resolveFieldAccesses(const Placement&, Function&, Vector<ValueID>& valuesWithOtherWords);
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
