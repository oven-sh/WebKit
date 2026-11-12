/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionData.h"
#include "AOTInstanceLayout.h"
#include "AOTLayoutData.h"
#include "AOTModuleData.h"
#include "AOTOperationData.h"
#include "AOTPagedBitSet.h"
#include "AOTRuntimeType.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <compare>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/Assertions.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct TypeSiteData {
    static constexpr uint32_t noText = (1u << 24) - 1;
    uint32_t site;
    uint32_t module;
    uint32_t position;
    uint32_t kind : 8;
    uint32_t text : 24;
};
static_assert(sizeof(TypeSiteData) == 16 && isImageRecord<TypeSiteData>);

struct PropertyRunHeader {
    uint32_t count : 16;
    uint32_t unused : 16 { 0 };

    uint32_t encoded() const { return std::bit_cast<uint32_t>(*this); }
    static PropertyRunHeader decode(uint32_t word) { return std::bit_cast<PropertyRunHeader>(word); }
};
static_assert(sizeof(PropertyRunHeader) == 4 && isImageRecord<PropertyRunHeader>);

struct TableRange {
    uint32_t offset;
    uint32_t count;
};
static_assert(sizeof(TableRange) == 8 && isImageRecord<TableRange>);

struct MethodTableDependentData {
    uint32_t guardBit;
    uint32_t distanceToEntry;
    uint32_t selector;

    friend auto operator<=>(const MethodTableDependentData&, const MethodTableDependentData&) = default;
};
static_assert(sizeof(MethodTableDependentData) == 12 && isImageRecord<MethodTableDependentData>);

struct ClosurePropertyNameData {
    uint32_t function;
    uint32_t name;

    friend auto operator<=>(const ClosurePropertyNameData&, const ClosurePropertyNameData&) = default;
};
static_assert(sizeof(ClosurePropertyNameData) == 8 && isImageRecord<ClosurePropertyNameData>);

struct AddedPrototypeNameData {
    uint32_t classLayout;
    uint32_t name;

    friend auto operator<=>(const AddedPrototypeNameData&, const AddedPrototypeNameData&) = default;
};
static_assert(sizeof(AddedPrototypeNameData) == 8 && isImageRecord<AddedPrototypeNameData>);

constexpr uint32_t distanceToMethodTableEntry(uint32_t distanceToWordOfSelector, uint32_t shape) { return distanceToWordOfSelector - shape * static_cast<uint32_t>(sizeof(uint32_t)); }
constexpr uint32_t methodTableEntryOf(uint32_t originOfSelector, uint32_t shape) { return originOfSelector - shape; }

#define FOR_EACH_AOT_OBJECT_PROGRAM_TABLE(v) \
    v(types, uint32_t) \
    v(typeSites, TypeSiteData) \
    v(layouts, uint32_t) \
    v(classes, uint32_t) \
    v(layoutSets, LayoutSetData) \
    v(allocationRows, AllocationRow) \
    v(allocationForms, AllocationData) \
    v(constructedAllocations, ConstructedAllocationData) \
    v(shapeSetBits, uint64_t) \
    v(propertyRuns, uint32_t) \
    v(fieldTable, uint32_t) \
    v(fieldRecordIndices, uint16_t) \
    v(classMemberRows, uint32_t) \
    v(classAccessors, uint64_t) \
    v(methodTableDependents, MethodTableDependentData) \
    v(classGuardCounts, ClassGuardCountData) \
    v(namesOfClassGuards, uint32_t) \
    v(classesBelowWithGuardsOfNames, ClassBelowWithGuardOfNameData) \
    v(assignedMembers, AssignedMemberData) \
    v(closurePropertyNames, ClosurePropertyNameData) \
    v(addedPrototypeNames, AddedPrototypeNameData) \
    v(parsedRecordNames, uint32_t) \
    v(parsedRecordStarts, uint32_t) \
    v(parsedRecordShapes, Number24)

#define FOR_EACH_AOT_PROGRAM_TABLE(v) \
    FOR_EACH_AOT_OBJECT_PROGRAM_TABLE(v) \
    FOR_EACH_AOT_FUNCTION_PROGRAM_TABLE(v) \
    FOR_EACH_AOT_OPERATION_PROGRAM_TABLE(v) \
    FOR_EACH_AOT_MODULE_PROGRAM_TABLE(v)

enum class ProgramTableIndex : uint8_t {
#define AOT_PROGRAM_TABLE_INDEX(accessor, ElementType) accessor,
    FOR_EACH_AOT_PROGRAM_TABLE(AOT_PROGRAM_TABLE_INDEX)
#undef AOT_PROGRAM_TABLE_INDEX
};

#define AOT_COUNT_PROGRAM_TABLE(accessor, ElementType) + 1
static constexpr unsigned numberOfProgramTables = 0 FOR_EACH_AOT_PROGRAM_TABLE(AOT_COUNT_PROGRAM_TABLE);
#undef AOT_COUNT_PROGRAM_TABLE

#define AOT_CHECK_PROGRAM_TABLE_ELEMENT(accessor, ElementType) static_assert(isImageRecord<ElementType>);
FOR_EACH_AOT_PROGRAM_TABLE(AOT_CHECK_PROGRAM_TABLE_ELEMENT)
#undef AOT_CHECK_PROGRAM_TABLE_ELEMENT

struct ProgramTables {
    static constexpr uint32_t expectedMagic = 0x44505943;
    static constexpr uint32_t mayRunInMultipleInstances = 1;
    static constexpr uint32_t neverReadsLegacyRegExpStatics = 2;
    static constexpr uint32_t shiftOfNearTypeNumbers = 2;

    uint32_t magic;
    uint32_t flags;
    InstanceSizes instanceSizes;
    uint32_t numberOfShapes;
    uint32_t firstEnvironmentShape;
    uint32_t numberOfLayoutIndices;
    uint32_t numberOfFieldTableRows;
    uint32_t numberOfFieldTableColumns;
    uint32_t numberOfClosureCells;
    uint32_t sizeOfStructuresOfHeirs;
    uint32_t shiftOfTypeNumbers;
    ClosureFunctionRangeData closureFunctions;
    TableRange ranges[numberOfProgramTables];

    template<typename T> const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(this) + offset); }
    template<typename T> uint32_t offsetOf(const T* record) const { return static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(record) - reinterpret_cast<const uint8_t*>(this)); }
    template<typename T> std::span<const T> table(ProgramTableIndex index) const
    {
        auto range = ranges[static_cast<unsigned>(index)];
        return { at<T>(range.offset), range.count };
    }
#define AOT_PROGRAM_TABLE_ACCESSOR(accessor, ElementType) \
    std::span<const ElementType> accessor() const { return table<ElementType>(ProgramTableIndex::accessor); }
    FOR_EACH_AOT_PROGRAM_TABLE(AOT_PROGRAM_TABLE_ACCESSOR)
#undef AOT_PROGRAM_TABLE_ACCESSOR

    static constexpr uint32_t memberAskedByByteAlone = 1u << 30;
    static constexpr uint32_t unlockedBuiltinMember = 1u << 31;
    static constexpr uint32_t openedName = 3u << 30;
    size_t numberOfWordsBelow(uint32_t mark) const
    {
        std::span<const uint32_t> members = constantAndUnlockedBuiltinMembers();
        return std::ranges::lower_bound(members, mark) - members.begin();
    }
    std::span<const uint32_t> constantBuiltinMembers() const { return constantAndUnlockedBuiltinMembers().first(numberOfWordsBelow(memberAskedByByteAlone)); }
    std::span<const uint32_t> membersAskedByByteAlone() const { return constantAndUnlockedBuiltinMembers().first(numberOfWordsBelow(unlockedBuiltinMember)).subspan(numberOfWordsBelow(memberAskedByByteAlone)); }
    std::span<const uint32_t> unlockedBuiltinMembers() const { return constantAndUnlockedBuiltinMembers().first(numberOfWordsBelow(openedName)).subspan(numberOfWordsBelow(unlockedBuiltinMember)); }
    std::span<const uint32_t> openedNames() const { return constantAndUnlockedBuiltinMembers().subspan(numberOfWordsBelow(openedName)); }
    static bool isUnlocked(std::span<const uint32_t> unlockedBuiltinMembers, uint32_t member) { return std::ranges::binary_search(unlockedBuiltinMembers, member | unlockedBuiltinMember); }
    static constexpr uint32_t numberOfNameTakenForAbsent(std::span<const uint8_t> utf8)
    {
        uint32_t number = 2166136261u;
        for (uint8_t byte : utf8)
            number = (number ^ byte) * 16777619u;
        return number;
    }

    template<typename Row>
    static const Row& findRequiredEntry(std::span<const Row> rows, uint32_t number, const auto& numberOf)
    {
        auto found = std::ranges::lower_bound(rows, number, { }, numberOf);
        RELEASE_ASSERT(found != rows.end() && static_cast<uint32_t>(std::invoke(numberOf, *found)) == number);
        return *found;
    }
    AllocationTable allocations() const { return { allocationRows(), allocationForms() }; }
    uint32_t constructorOfAllocation(uint32_t number) const
    {
        if (!allocations()[number].isConstructed)
            return AllocationData::notConstructed;
        return findRequiredEntry(constructedAllocations(), number, [](const ConstructedAllocationData& entry) -> uint32_t { return entry.allocation; }).constructedBy;
    }
    bool hasLayoutForShape(uint32_t shape) const { return shape < firstEnvironmentShape && layouts()[shape]; }
    const LayoutData& layoutForShape(uint32_t shape) const
    {
        ASSERT(hasLayoutForShape(shape));
        return *at<LayoutData>(layouts()[shape]);
    }
    std::span<const uint32_t> fieldTableEntries() const { return fieldTable().subspan(numberOfFieldTableRows); }
    uint32_t fieldTableRowOf(uint32_t shape) const { return fieldTable()[numberOfFieldTableRows - shape]; }
    bool hasFieldTableColumn(uint32_t identifierNumber) const { return identifierNumber && identifierNumber < numberOfFieldTableColumns; }
    const EnvironmentData& environmentForShape(uint32_t shape) const
    {
        ASSERT(shape >= firstEnvironmentShape);
        return *at<EnvironmentData>(layouts()[shape]);
    }
    const TypeSiteData* findTypeSite(uint32_t site) const
    {
        std::span<const TypeSiteData> sites = typeSites();
        auto found = std::ranges::lower_bound(sites, site, { }, &TypeSiteData::site);
        return found != sites.end() && found->site == site ? &*found : nullptr;
    }
    uint32_t numberOfTypes() const { return types()[0]; }
    static constexpr uint32_t numberOfTypeGroupWords(uint32_t numberOfTypes) { return (numberOfTypes + 31) / 32 * 2; }
    uint32_t offsetOfType(uint32_t index) const
    {
        const uint32_t* groups = types().data() + 1;
        uint32_t haveRecord = groups[index / 32 * 2];
        if (!(haveRecord >> index % 32 & 1))
            return 0;
        return groups[numberOfTypeGroupWords(numberOfTypes()) + groups[index / 32 * 2 + 1] + std::popcount(haveRecord & ((1u << index % 32) - 1))];
    }
    const RuntimeType& typeAt(uint32_t offset) const { return *at<RuntimeType>(offset); }
    uint32_t offsetOfTypeOf(const FieldData& field) const { return static_cast<uint32_t>(field.typeNumber) << shiftOfTypeNumbers; }
    const ShapeTestData& shapeTestAt(uint32_t offset) const { return *at<ShapeTestData>(offset); }
    const KeyOrderData& keyOrder(const LayoutData& layout, uint32_t order) const
    {
        ASSERT(order < layout.numberOfOrders);
        return *at<KeyOrderData>(at<uint32_t>(layout.ordersOffset)[order]);
    }
    bool passes(const ShapeTestData& test, uint32_t shape) const
    {
        uint32_t indexFromLast = test.last - shape;
        if (indexFromLast >= test.count)
            return false;
        if (test.bitSetOffset == noBitSet)
            return true;
        return PagedBitSet::contains(shapeSetBits().data() + PagedBitSet::firstWordAt(test.bitSetOffset), PagedBitSet::entriesAt(test.bitSetOffset), indexFromLast);
    }
    uint32_t numberOfClassGroupWords() const { return (numberOfLayoutIndices + 31) / 32 * 2; }
    uint32_t numberOfClasses() const { return classes().size() - numberOfClassGroupWords(); }
    uint32_t rankOfClass(uint32_t index) const
    {
        const uint32_t* groups = classes().data();
        return groups[index / 32 * 2 + 1] + std::popcount(groups[index / 32 * 2] & ((1u << index % 32) - 1));
    }
    uint32_t offsetOfClass(uint32_t index) const
    {
        const uint32_t* groups = classes().data();
        if (!(groups[index / 32 * 2] >> index % 32 & 1))
            return 0;
        return groups[numberOfClassGroupWords() + rankOfClass(index)];
    }
    bool someClassGuardNames(uint32_t identifierNumber) const
    {
        std::span<const uint32_t> words = namesOfClassGuards();
        return identifierNumber / 32 < words.size() && (words[identifierNumber / 32] >> identifierNumber % 32 & 1);
    }
    std::span<const uint32_t> propertyRunRow(uint32_t number) const
    {
        const uint32_t* starts = propertyRuns().data();
        return { starts + starts[number], starts[number + 1] - starts[number] };
    }
    const FunctionWithLargeCountsData& largeCountsOf(uint32_t function) const
    {
        return findRequiredEntry(functionsWithLargeCounts(), function, [](const FunctionWithLargeCountsData& entry) -> uint32_t { return entry.function; });
    }
    bool hasClosureNumber(uint32_t number) const { return number < closureFunctions.end - closureFunctions.first; }
    uint32_t closureNumberOf(uint32_t function) const
    {
        ASSERT(function >= closureFunctions.first && function < closureFunctions.end);
        return function - closureFunctions.first;
    }
    uint32_t closureFunctionWithNumber(uint32_t number) const
    {
        ASSERT(hasClosureNumber(number));
        return closureFunctions.first + number;
    }
    const ClosureFacts& factsOfClosureNumber(uint32_t number) const { return closureFacts()[closureFactNumbers()[number]]; }
    uint32_t functionLengthOf(uint32_t function) const
    {
        uint8_t length = factsOfClosureNumber(function - closureFunctions.first).length;
        return length == countsAreListed ? largeCountsOf(function).length() : length;
    }
    CaptureCounts captureCountsOfClosureNumber(uint32_t number) const
    {
        uint8_t counts = factsOfClosureNumber(number).captureCounts;
        if (counts == countsAreListed) [[unlikely]]
            return largeCountsOf(closureFunctionWithNumber(number)).captureCounts;
        return CaptureCounts::decode(counts);
    }
    CaptureCounts captureCountsOf(uint32_t function) const { return captureCountsOfClosureNumber(closureNumberOf(function)); }
    bool closureNumberOnlyReturns(uint32_t number) const
    {
        auto entries = constructEntrypoints();
        auto entry = std::ranges::lower_bound(entries, number, { }, &ConstructEntrypointData::function);
        return entry != entries.end() && entry->function == number && entry->constructEntrypointAndFlag == ConstructEntrypointData::onlyReturns;
    }
    FunctionKindData kindOfClosureNumber(uint32_t number) const { return factsOfClosureNumber(number).kind; }
    size_t numberOfFunctions() const { return functionRecordNumbers().size(); }
    const FunctionRecordData& recordOfFunction(uint32_t function) const
    {
        uint8_t number = functionRecordNumbers()[function];
        if (number != unsharedFunctionRecord) [[likely]]
            return functionRecords()[number];
        return unsharedRecordOfFunction(function);
    }
    bool areUnsharedRecordsConsistent() const
    {
        std::span<const uint8_t> numbers = functionRecordNumbers();
        return std::ranges::all_of(unsharedFunctionRecords(), [&](const UnsharedFunctionRecordData& entry) {
            return entry.function < numbers.size() && numbers[entry.function] == unsharedFunctionRecord;
        });
    }
    NEVER_INLINE const FunctionRecordData& unsharedRecordOfFunction(uint32_t function) const
    {
        static constinit std::atomic<const ProgramTables*> checkedTables { nullptr };
        if (checkedTables.load(std::memory_order_relaxed) != this) [[unlikely]] {
            RELEASE_ASSERT(areUnsharedRecordsConsistent());
            checkedTables.store(this, std::memory_order_relaxed);
        }
        return findRequiredEntry(unsharedFunctionRecords(), function, &UnsharedFunctionRecordData::function).record;
    }
    const FunctionTraceData& traceOfFunction(uint32_t function) const { return recordOfFunction(function).trace; }
    FunctionKindData kindOfFunction(uint32_t function) const
    {
        uint32_t index = function - closureFunctions.first;
        if (index < closureFactNumbers().size()) [[likely]]
            return kindOfClosureNumber(index);
        return recordOfFunction(function).kind;
    }
    uint32_t sourceFunctionOf(uint32_t function) const
    {
        std::span<const uint32_t> sources = sourceFunctions();
        return sources.empty() || function < closureFunctions.first ? function : AscendingNumberTable(sources).at(function - closureFunctions.first);
    }
    std::span<const Number24> parsedRecordShapesWithName(uint32_t name) const
    {
        uint32_t place = parsedRecordNames().empty() ? 0 : SparseNumberTable(parsedRecordNames()).at(name);
        if (!place)
            return { };
        AscendingNumberTable starts(parsedRecordStarts());
        uint32_t first = starts.at(place - 1);
        return parsedRecordShapes().subspan(first, starts.at(place) - first);
    }
    uint32_t cellOfClosureFunction(uint32_t function) const
    {
        return function >= closureFunctions.first && function < closureFunctions.end ? SparseNumberTable(closureCells()).at(function - closureFunctions.first) : 0;
    }
    static constexpr uint32_t numberOfFunctionNameGroupWords(size_t numberOfFunctions) { return static_cast<uint32_t>((numberOfFunctions + 31) / 32 * 2); }
    std::optional<uint32_t> nameOfFunction(uint32_t function) const
    {
        uint32_t source = sourceFunctionOf(function);
        const uint32_t* group = functionNameGroups().data() + source / 32 * 2;
        if (!(group[0] >> source % 32 & 1))
            return std::nullopt;
        return functionNames()[group[1] + std::popcount(group[0] & ((1u << source % 32) - 1))];
    }
    std::optional<uint32_t> builtinImplementedBy(uint32_t function) const
    {
        if (!traceOfFunction(function).implementsBuiltin)
            return std::nullopt;
        return findRequiredEntry(implementedBuiltins(), function, &ImplementedBuiltinData::function).builtin;
    }
};
static_assert(std::is_trivially_copyable_v<ProgramTables> && isImageRecord<ProgramTables>);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
