/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCoroutineData.h"
#include "AOTFunctionData.h"
#include "AOTInstanceLayout.h"
#include "AOTLayoutData.h"
#include "AOTModuleData.h"
#include "AOTOperationData.h"
#include "AOTRuntimeType.h"
#include <algorithm>
#include <bit>
#include <span>
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

// Only a validating image has any, in ascending order.
struct RemovedFieldData {
    static constexpr uint32_t everyObjectHadOne = std::numeric_limits<uint32_t>::max();

    uint32_t layout;
    uint32_t name;

    friend auto operator<=>(const RemovedFieldData&, const RemovedFieldData&) = default;
};
static_assert(sizeof(RemovedFieldData) == 8 && isImageRecord<RemovedFieldData>);

// Only a validating image has any, in ascending order: for each layout whose list is complete, the names that its objects get beside their fields,
// and in a table of its own for each class whose list is complete, the names that its prototype object ever has beside `constructor`.
struct AddedNameData {
    static constexpr uint32_t listIsComplete = std::numeric_limits<uint32_t>::max();

    uint32_t layout;
    uint32_t name;

    friend auto operator<=>(const AddedNameData&, const AddedNameData&) = default;
};
static_assert(sizeof(AddedNameData) == 8 && isImageRecord<AddedNameData>);

#define FOR_EACH_AOT_OBJECT_PROGRAM_TABLE(v) \
    v(types, uint32_t) \
    v(typeSites, TypeSiteData) \
    v(layouts, uint32_t) \
    v(classes, uint32_t) \
    v(layoutSets, LayoutSetData) \
    v(allocations, AllocationData) \
    v(shapeSetBits, uint32_t) \
    v(propertyRuns, uint32_t) \
    v(fieldTable, uint32_t) \
    v(fieldRecordIndices, uint16_t) \
    v(removedFields, RemovedFieldData) \
    v(addedNames, AddedNameData) \
    v(classPrototypeNames, AddedNameData)

#define FOR_EACH_AOT_PROGRAM_TABLE(v) \
    FOR_EACH_AOT_OBJECT_PROGRAM_TABLE(v) \
    FOR_EACH_AOT_FUNCTION_PROGRAM_TABLE(v) \
    FOR_EACH_AOT_COROUTINE_PROGRAM_TABLE(v) \
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

    uint32_t magic;
    uint32_t flags;
    InstanceSizes instanceSizes;
    uint32_t numberOfShapes;
    uint32_t firstEnvironmentShape;
    uint32_t numberOfLayoutIndices;
    uint32_t numberOfFieldTableRows;
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

    bool hasLayoutForShape(uint32_t shape) const { return shape < firstEnvironmentShape && layouts()[shape]; }
    const LayoutData& layoutForShape(uint32_t shape) const
    {
        ASSERT(hasLayoutForShape(shape));
        return *at<LayoutData>(layouts()[shape]);
    }
    std::span<const uint32_t> fieldTableEntries() const { return fieldTable().subspan(numberOfFieldTableRows); }
    uint32_t fieldTableRowOf(uint32_t shape) const { return fieldTable()[numberOfFieldTableRows - shape]; }
    const EnvironmentData& environmentForShape(uint32_t shape) const
    {
        ASSERT(shape >= firstEnvironmentShape);
        return *at<EnvironmentData>(layouts()[shape]);
    }
    // The table has the sites that a value or a static field refers to, in ascending order. Only messages ask.
    const TypeSiteData* findTypeSite(uint32_t site) const
    {
        std::span<const TypeSiteData> sites = typeSites();
        auto found = std::ranges::lower_bound(sites, site, { }, &TypeSiteData::site);
        return found != sites.end() && found->site == site ? &*found : nullptr;
    }
    // The number of types, two words for every 32 of them (which have a record, how many in front of them have one), then the offsets of the records.
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
    const ShapeTestData& shapeTestAt(uint32_t offset) const { return *at<ShapeTestData>(offset); }
    const KeyOrderData& keyOrder(const LayoutData& layout, uint32_t order) const
    {
        ASSERT(order < layout.numberOfOrders);
        return *at<KeyOrderData>(at<uint32_t>(layout.ordersOffset)[order]);
    }
    bool passes(const ShapeTestData& test, uint32_t shape) const
    {
        uint32_t indexFromLast = test.first + test.count - 1 - shape;
        if (indexFromLast >= test.count)
            return false;
        if (test.bitSetOffset == noBitSet)
            return true;
        uint32_t bit = test.bitSetOffset + indexFromLast;
        return shapeSetBits()[bit / 32] >> bit % 32 & 1;
    }
    uint32_t numberOfClassGroupWords() const { return (numberOfLayoutIndices + 31) / 32 * 2; }
    uint32_t numberOfClasses() const { return classes().size() - numberOfClassGroupWords(); }
    // How many classes have a smaller layout index.
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
    std::span<const uint32_t> propertyRunRow(uint32_t number) const
    {
        const uint32_t* starts = propertyRuns().data();
        return { starts + starts[number], starts[number + 1] - starts[number] };
    }
    uint32_t functionLengthOf(uint32_t function) const { return functionLengths()[function - closureFunctions.first]; }
    // Every function in front of the first one that is a form of another is its own source. The table starts at that one.
    uint32_t sourceFunctionOf(uint32_t function) const
    {
        std::span<const uint32_t> sources = functionSources();
        size_t firstListed = functionTraces().size() - sources.size();
        return function < firstListed ? function : sources[function - firstListed];
    }
    std::optional<uint32_t> builtinImplementedBy(uint32_t function) const
    {
        if (!functionTraces()[function].implementsBuiltin)
            return std::nullopt;
        return std::ranges::lower_bound(implementedBuiltins(), function, { }, &ImplementedBuiltinData::function)->builtin;
    }
};
static_assert(std::is_trivially_copyable_v<ProgramTables> && isImageRecord<ProgramTables>);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
