/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTDerivedFunctions.h"
#include "AOTRuntimeType.h"
#include "AOTSchemaWalkers.h"
#include "AOTThreads.h"
#include "AOTTypeTestPlan.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <wtf/FastBitVector.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maxEncodedCellTests = std::numeric_limits<decltype(RuntimeType::numberOfCellTests)>::max();
static constexpr size_t maxEncodedLiteralStrings = std::numeric_limits<decltype(RuntimeType::numberOfLiteralStrings)>::max();

static_assert(!offsetof(LayoutSetData, members));

using RuntimeTypeWords = Vector<uint32_t, 16>;

namespace {

struct PrintedName {
    enum class Kind : uint8_t { LayoutName, AllFieldNames, FirstFieldNames };
    static constexpr size_t maxFieldNames = 4;

    Kind kind { Kind::AllFieldNames };
    std::array<StringID, maxFieldNames> strings;

    friend bool operator==(const PrintedName&, const PrintedName&) = default;
};

struct LayoutSetRecords {
    std::optional<uint32_t> recordOf(LayoutSetID set) const { return set ? recordOfSet[set.index()] : std::nullopt; }
    uint32_t shapeTestOffsetOf(uint32_t record) const { return tableOffset + record * static_cast<uint32_t>(sizeof(LayoutSetData)); }

    Vector<LayoutSetID> sets;
    Vector<std::optional<uint32_t>> recordOfSet;
    uint32_t tableOffset { 0 };
};

struct PlannedType {
    TypeID id;
    TypeTestPlan plan;
};

} // anonymous namespace

bool retainsDeclarationLocation(TableEncoder& encoder, LayoutID id)
{
    return !encoder.obfuscates() && encoder.program.at(id).module && encoder.layoutsWithSharedName.get(id.index());
}

DeclarationLocation declarationLocationOf(TableEncoder& encoder, LayoutID id)
{
    if (!retainsDeclarationLocation(encoder, id))
        return { };
    const Layout& layout = encoder.program.at(id);
    const Module& declaringModule = encoder.program.at(layout.module);
    uint32_t path = encoder.stringNumber(declaringModule.displayPath ? declaringModule.displayPath : declaringModule.path);
    return { path, encoder.program.lineColumnFor(layout.module, layout.position).line };
}

static PrintedName printedNameOf(const Program& program, const SlotPlan& slots, LayoutID id)
{
    const Layout& layout = program.at(id);
    PrintedName name;
    if (layout.name && !program.string(layout.name).empty()) {
        name.kind = PrintedName::Kind::LayoutName;
        name.strings[0] = layout.name;
        return name;
    }
    bool hasSlots = slots.layout(id).hasSlots();
    size_t numberOfFieldNames = 0;
    for (FieldIndex index = 0; index < layout.fields.size(); ++index) {
        PropertyKeyID key = layout.fields[index].key;
        if (hasSlots ? !slots.slot(id, index).hasStorage() : !key)
            continue;
        if (numberOfFieldNames == PrintedName::maxFieldNames) {
            name.kind = PrintedName::Kind::FirstFieldNames;
            break;
        }
        StringID fieldName = key ? program.at(key).text : StringID();
        name.strings[numberOfFieldNames++] = fieldName ? fieldName : program.strings.emptyString();
    }
    return name;
}

static BitVector findLayoutsWithSharedName(const Program& program, const SlotPlan& slots)
{
    struct FirstUse {
        PrintedName name;
        LayoutID layout;
    };
    Vector<FirstUse> firstUses;
    HashedIndex firstUseIndex;
    BitVector layoutsWithSharedName(program.layouts.size());
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        if (!isObjectLayout(program.at(id).kind))
            continue;
        PrintedName name = printedNameOf(program, slots, id);
        uint32_t hash = computeHash(name.kind, name.strings);
        std::optional<uint32_t> firstUse = firstUseIndex.find(hash, [&](uint32_t position) {
            return firstUses[position].name == name;
        });
        if (!firstUse) {
            firstUseIndex.add(hash, static_cast<uint32_t>(firstUses.size()));
            firstUses.append({ name, id });
            continue;
        }
        layoutsWithSharedName.quickSet(firstUses[*firstUse].layout.index());
        layoutsWithSharedName.quickSet(index);
    }
    return layoutsWithSharedName;
}

static LayoutSetRecords numberLayoutSets(const Program& program, const BitVector& neededTypes)
{
    unsigned numberOfSets = program.types.numberOfLayoutSets();
    BitVector setsInNeededTypes(numberOfSets);
    for (size_t index : neededTypes) {
        if (LayoutSetID set = program.at(TypeID(index)).layouts)
            setsInNeededTypes.quickSet(set.index());
    }
    LayoutSetRecords records;
    records.recordOfSet = Vector<std::optional<uint32_t>>(FillWith { }, numberOfSets, std::nullopt);
    for (size_t index : setsInNeededTypes) {
        records.recordOfSet[index] = static_cast<uint32_t>(records.sets.size());
        records.sets.append(LayoutSetID(index));
    }
    return records;
}

static Vector<LayoutID> findNamingLayouts(const Program& program)
{
    Vector<LayoutID> namingLayouts(FillWith { }, program.types.numberOfLayoutSets(), LayoutID());
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        if (!isObjectLayout(program.at(id).kind))
            continue;
        LayoutSetID set = program.declaredMembersOf(id);
        // All layouts without members share one set, so the name of one of them would be printed for the others.
        if (!set || program.at(set).members.isEmpty() || namingLayouts[set.index()])
            continue;
        namingLayouts[set.index()] = id;
    }
    return namingLayouts;
}

static void encodeLayoutSets(TableEncoder& encoder, LayoutSetRecords& records)
{
    const Program& program = encoder.program;
    const ShapeTable& shapes = encoder.placement.shapes();
    Vector<LayoutID> namingLayouts = findNamingLayouts(program);
    encoder.setTable(ProgramTableIndex::layoutSets, Vector<LayoutSetData>(records.sets.size(), [&](size_t record) {
        LayoutSetID set = records.sets[record];
        LayoutID namingLayout = namingLayouts[set.index()];
        DeclarationLocation location = namingLayout ? declarationLocationOf(encoder, namingLayout) : DeclarationLocation { };
        return LayoutSetData {
            .members = shapes.dataOf(shapes.testOf(set)),
            .name = encoder.stringNumber(namingLayout ? program.at(namingLayout).name : StringID()),
            .declaredPath = location.path,
            .declaredLine = location.line,
        };
    }));
    records.tableOffset = encoder.header.ranges[static_cast<unsigned>(ProgramTableIndex::layoutSets)].offset;
}

static uint32_t shapeTestOffsetOf(TableEncoder& encoder, const LayoutSetRecords& records, LayoutSetID set)
{
    if (std::optional<uint32_t> record = records.recordOf(set))
        return records.shapeTestOffsetOf(*record);
    const ShapeTable& shapes = encoder.placement.shapes();
    const ShapeTestData test = shapes.dataOf(shapes.testOf(set));
    return encoder.writer.append(singleElementSpan(test));
}

static std::optional<RuntimeCellTest> encodeCellTest(TableEncoder& encoder, const LayoutSetRecords& records, const CellTest& test)
{
    switch (test.kind) {
    case CellTestKind::IsObject:
        return RuntimeCellTest::isObject();
    case CellTestKind::IsCallable:
        return RuntimeCellTest::isCallable();
    case CellTestKind::CellTypeIs:
        return RuntimeCellTest::cellTypeIs(test.cellTypes.first, test.cellTypes.last);
    case CellTestKind::ShapeIn:
        return RuntimeCellTest::shapeIn(shapeTestOffsetOf(encoder, records, test.layouts));
    case CellTestKind::InheritsClassInfo:
        return RuntimeCellTest::inheritsClassInfo(test.builtinClass);
    case CellTestKind::IsPristine:
        return RuntimeCellTest::isPristine(test.builtinClass);
    case CellTestKind::IsBuiltinCell: {
        std::optional<uint32_t> cell = encoder.placement.builtinCell(test.builtin);
        if (!cell)
            encoder.fail(ProgramTableRule::TypeNotEncoded);
        return RuntimeCellTest::isBuiltinCell(cell.value_or(0));
    }
    case CellTestKind::IsClosureOf: {
        // A closure names the generic entrypoint of its function. A function without one has no closures, so no cell passes the test.
        FunctionID entrypoint = encoder.findEntrypoint(test.function);
        if (!entrypoint)
            return std::nullopt;
        return RuntimeCellTest::isClosureOf(entrypoint);
    }
    case CellTestKind::IsSymbol:
        return RuntimeCellTest::isSymbol(encoder.identifierNumber(test.symbol));
    case CellTestKind::SharingModeIs:
        return RuntimeCellTest::sharingModeIs(test.sharingMode);
    case CellTestKind::IsNamespaceOf:
        return RuntimeCellTest::isNamespaceOf(test.module);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID describedContentsOf(const Program& program, TypeID id)
{
    const Type& type = program.at(id);
    if (type.classes.size() != 1)
        return { };
    TypeID contents = type.classes[0].contentsAt(program.builtins(), ContentsSlot::Element);
    if (contents == TypeTable::anything)
        return { };
    return contents;
}

static std::optional<uint32_t> findContentsOffset(const TableEncoder& encoder, TypeID id)
{
    TypeID contents = describedContentsOf(encoder.program, id);
    if (!contents)
        return std::nullopt;
    uint32_t offset = encoder.typeOffsets[contents.index()];
    if (!offset)
        return std::nullopt;
    return offset;
}

template<typename Record>
static void appendRecord(RuntimeTypeWords& words, const Record& record)
{
    auto recordWords = std::bit_cast<std::array<uint32_t, sizeof(Record) / sizeof(uint32_t)>>(record);
    words.append(std::span { recordWords });
}

static void appendInt64(RuntimeTypeWords& words, int64_t integer)
{
    words.appendList({ static_cast<uint32_t>(integer), static_cast<uint32_t>(static_cast<uint64_t>(integer) >> 32) });
}

// In a plan, String says that some string passes. In a record it says that every string passes.
static uint16_t encodePrimitives(const TypeTestPlan& plan)
{
    if (plan.stringLiterals.isEmpty())
        return plan.passingPrimitives;
    return (plan.passingPrimitives & ~Primitive::String) | Primitive::LiteralString;
}

static void appendRuntimeType(TableEncoder& encoder, const LayoutSetRecords& records, const PlannedType& planned)
{
    const TypeTestPlan& plan = planned.plan;
    RuntimeType header { };
    Vector<RuntimeCellTest, 4> cellTests;
    std::span<const StringID> literalStrings;
    std::optional<Range> integers;
    std::optional<uint32_t> contentsOffset;
    std::optional<uint32_t> layoutSet;
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        header.primitives = encoder.program.at(TypeTable::anything).primitives;
        cellTests.append(RuntimeCellTest::isObject());
        break;
    case TypeTestVerdict::NeverPasses:
        break;
    case TypeTestVerdict::NeedsTest:
        header.primitives = encodePrimitives(plan);
        for (const CellTest& test : plan.cellTests) {
            if (std::optional<RuntimeCellTest> encoded = encodeCellTest(encoder, records, test))
                cellTests.append(*encoded);
        }
        literalStrings = plan.stringLiterals.span();
        integers = plan.integers;
        contentsOffset = findContentsOffset(encoder, planned.id);
        layoutSet = records.recordOf(encoder.program.at(planned.id).layouts);
        break;
    }
    if (cellTests.size() > maxEncodedCellTests || literalStrings.size() > maxEncodedLiteralStrings) {
        encoder.fail(ProgramTableRule::TypeNotEncoded);
        return;
    }
    header.flags = (integers ? RuntimeType::hasIntegerRange : 0) | (contentsOffset ? RuntimeType::hasContents : 0) | (layoutSet ? RuntimeType::hasLayoutSet : 0);
    header.numberOfCellTests = static_cast<uint16_t>(cellTests.size());
    header.numberOfLiteralStrings = static_cast<uint16_t>(literalStrings.size());

    RuntimeTypeWords words;
    appendRecord(words, header);
    for (const RuntimeCellTest& test : cellTests)
        appendRecord(words, test);
    for (StringID string : literalStrings)
        words.append(encoder.stringNumber(string));
    if (integers) {
        appendInt64(words, integers->minimum);
        appendInt64(words, integers->maximum);
    }
    if (contentsOffset)
        words.append(*contentsOffset);
    if (layoutSet)
        words.append(*layoutSet);
    RELEASE_ASSERT(words.sizeInBytes() == RuntimeType::sizeInBytes(header.numberOfCellTests, header.numberOfLiteralStrings, header.flags));
    encoder.typeOffsets[planned.id.index()] = encoder.writer.append(words);
}

static const PlannedType* findPlannedType(std::span<const PlannedType> plannedTypes, TypeID id)
{
    if (!id)
        return nullptr;
    auto found = std::ranges::lower_bound(plannedTypes, id, { }, &PlannedType::id);
    if (found == plannedTypes.end() || found->id != id)
        return nullptr;
    return &*found;
}

static void encodeTypes(TableEncoder& encoder, const LayoutSetRecords& records)
{
    const Program& program = encoder.program;
    const BitVector& neededTypes = encoder.placement.typesNeededAtRunTime();
    auto plannedTypes = collectInParallel<PlannedType>(encoder.build.parallelLoops(), program.types.size(), [&](unsigned index, Vector<PlannedType>& collected) {
        TypeID id(index);
        if (neededTypes.get(index))
            collected.append({ id, computeTypeTestPlanFromAnything(program, program.at(id).withoutProofs()) });
    });
    encoder.typeOffsets = Vector<uint32_t>(FillWith { }, program.types.size(), 0u);
    Vector<const PlannedType*, 8> typesBeforeContents;
    for (const PlannedType& planned : plannedTypes) {
        for (const PlannedType* next = &planned; next && !encoder.typeOffsets[next->id.index()]; next = findPlannedType(plannedTypes.span(), describedContentsOf(program, next->id))) {
            RELEASE_ASSERT(typesBeforeContents.size() < plannedTypes.size());
            typesBeforeContents.append(next);
        }
        while (!typesBeforeContents.isEmpty())
            appendRuntimeType(encoder, records, *typesBeforeContents.takeLast());
    }
    uint32_t numberOfTypes = program.types.size();
    Vector<uint32_t> index(FillWith { }, 1 + ProgramTables::numberOfTypeGroupWords(numberOfTypes), 0u);
    index[0] = numberOfTypes;
    uint32_t numberOfRecords = 0;
    for (uint32_t type = 0; type < numberOfTypes; ++type) {
        if (!(type % 32))
            index[1 + type / 32 * 2 + 1] = numberOfRecords;
        if (uint32_t offset = encoder.typeOffsets[type]) {
            index[1 + type / 32 * 2] |= 1u << type % 32;
            index.append(offset);
            ++numberOfRecords;
        }
    }
    encoder.setTable(ProgramTableIndex::types, index);
}

static FastBitVector findUsedTypeSites(const Program& program, const ParallelLoops& loops)
{
    FastBitVector usedSites(program.typeSites.size());
    auto markUsed = [&](TypeSiteID site) {
        usedSites.atomicSetAndCheck(site.index(), true);
    };
    forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values)
                forEachTypeSite(function.at(id), markUsed);
            forEachTypeSite(block.terminator, markUsed);
        }
    });
    for (const Layout& layout : program.layouts) {
        for (const StaticField& field : layout.staticFields) {
            if (TypeSiteID site = program.at(field.variable).declared.site)
                markUsed(site);
        }
    }
    return usedSites;
}

static uint32_t encodeTypeSiteText(TableEncoder& encoder, const TypeSite& site, bool retainsText)
{
    if (!site.text || !retainsText)
        return TypeSiteData::noText;
    uint32_t text = encoder.stringNumber(site.text);
    if (text < TypeSiteData::noText)
        return text;
    encoder.build.statistics().count(StatisticID::TypeSiteTextsLeftOut);
    return TypeSiteData::noText;
}

static void encodeTypeSites(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    FastBitVector usedSites = findUsedTypeSites(program, encoder.build.parallelLoops());
    Vector<TypeSiteData> sites;
    sites.reserveInitialCapacity(usedSites.bitCount());
    usedSites.forEachSetBit([&](size_t index) {
        const TypeSite& site = program.typeSites[index];
        TypeSiteData data { };
        data.site = static_cast<uint32_t>(index);
        data.module = site.module.index();
        data.position = site.position;
        data.kind = static_cast<uint32_t>(site.kind);
        data.text = encodeTypeSiteText(encoder, site, !encoder.obfuscates());
        sites.append(data);
    });
    encoder.setTable(ProgramTableIndex::typeSites, sites);
}

void encodeTypeTables(TableEncoder& encoder)
{
    encoder.layoutsWithSharedName = findLayoutsWithSharedName(encoder.program, encoder.slots);
    LayoutSetRecords records = numberLayoutSets(encoder.program, encoder.placement.typesNeededAtRunTime());
    encodeLayoutSets(encoder, records);
    encodeTypes(encoder, records);
    encodeTypeSites(encoder);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
