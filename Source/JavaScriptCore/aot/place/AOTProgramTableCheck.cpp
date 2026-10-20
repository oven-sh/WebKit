/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTDerivedFunctions.h"
#include "AOTFieldTable.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeType.h"
#include "AOTThreads.h"
#include "AOTTypeTestPlan.h"
#include "IndexingType.h"
#include <algorithm>
#include <bit>
#include <compare>
#include <functional>
#include <string_view>
#include <tuple>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ExpectedRuntimeType {
    TypeID id;
    uint16_t primitives { 0 };
    std::optional<Range> integers;
    Vector<CellTest, 4> cellTests;
    Vector<StringID, 4> literalStrings;
};

} // anonymous namespace

template<typename Entry, typename KeyFunctor>
static bool isStrictlyAscending(std::span<const Entry> entries, const KeyFunctor& keyOf)
{
    for (size_t i = 1; i < entries.size(); ++i) {
        if (!(std::invoke(keyOf, entries[i - 1]) < std::invoke(keyOf, entries[i])))
            return false;
    }
    return true;
}

static std::string_view viewOf(std::span<const uint8_t> bytes)
{
    auto characters = byteCast<char>(bytes);
    return { characters.data(), characters.size() };
}

template<typename Record>
static const Record* tryGetRecord(std::span<const uint8_t> bytes, uint32_t offset)
{
    if (offset % alignof(Record) || offset < sizeof(ProgramTables) || offset + sizeof(Record) > bytes.size())
        return nullptr;
    return &reinterpretCastSpanStartTo<Record>(bytes.subspan(offset));
}

static bool checkTableBounds(const TableEncoder& encoder, std::span<const uint8_t> bytes)
{
    if (bytes.size() < sizeof(ProgramTables)) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return false;
    }
    auto& tables = reinterpretCastSpanStartTo<ProgramTables>(bytes);
    bool isInBounds = true;
    auto checkRange = [&](ProgramTableIndex index, size_t elementSize, size_t alignment) {
        if (!encoder.writtenTables.get(static_cast<unsigned>(index)))
            encoder.fail(ProgramTableRule::TableNotWritten);
        TableRange range = tables.ranges[static_cast<unsigned>(index)];
        if (!(range.offset % alignment) && range.offset + static_cast<uint64_t>(range.count) * elementSize <= bytes.size())
            return;
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        isInBounds = false;
    };
#define AOT_CHECK_PROGRAM_TABLE_RANGE(accessor, ElementType) checkRange(ProgramTableIndex::accessor, sizeof(ElementType), alignof(ElementType));
    FOR_EACH_AOT_PROGRAM_TABLE(AOT_CHECK_PROGRAM_TABLE_RANGE)
#undef AOT_CHECK_PROGRAM_TABLE_RANGE
    return isInBounds;
}

static void checkConstantBuiltinMembers(const TableEncoder& encoder, const ProgramTables& tables)
{
    const BuiltinTable& builtins = encoder.program.builtins();
    auto members = tables.constantBuiltinMembers();
    bool everyMemberExists = std::ranges::all_of(members, [&](uint32_t member) {
        return builtins.contains(BuiltinID(member));
    });
    if (!everyMemberExists) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    bool isSorted = isStrictlyAscending(members, [&](uint32_t member) {
        BuiltinID id(member);
        return std::pair { builtins.at(id).holder, id };
    });
    if (!isSorted)
        encoder.fail(ProgramTableRule::TableNotSorted);
}

static void checkClosedAbsentNames(const TableEncoder& encoder, const ProgramTables& tables)
{
    const BuiltinTable& builtins = encoder.program.builtins();
    auto holders = tables.holdersOfClosedAbsentNames();
    auto names = tables.closedAbsentNames();
    bool holdersAreSorted = isStrictlyAscending(holders, [](const ClosedAbsentNamesOfHolder& holder) {
        return holder.holder;
    });
    if (!holdersAreSorted)
        encoder.fail(ProgramTableRule::TableNotSorted);

    uint32_t numberOfNames = 0;
    for (const ClosedAbsentNamesOfHolder& holder : holders) {
        if (!builtins.contains(BuiltinID(holder.holder)) || holder.firstName != numberOfNames || holder.numberOfNames > names.size() - numberOfNames) {
            encoder.fail(ProgramTableRule::TableOutOfBounds);
            return;
        }
        auto namesOfHolder = names.subspan(holder.firstName, holder.numberOfNames);
        if (!isStrictlyAscending(namesOfHolder, std::identity { }))
            encoder.fail(ProgramTableRule::TableNotSorted);
        if (!namesOfHolder.empty() && (!namesOfHolder.front() || namesOfHolder.back() >= encoder.identifiers.count()))
            encoder.fail(ProgramTableRule::NameWithoutIdentifier);
        numberOfNames += holder.numberOfNames;
    }
    if (numberOfNames != names.size())
        encoder.fail(ProgramTableRule::TableOutOfBounds);
}

static void checkBuiltinCells(const TableEncoder& encoder, const ProgramTables& tables)
{
    if (!isStrictlyAscending(tables.builtinCells(), std::identity { }) || !isStrictlyAscending(tables.eagerBuiltinCells(), std::identity { }))
        encoder.fail(ProgramTableRule::TableNotSorted);
    if (!tables.eagerBuiltinCells().empty() && tables.eagerBuiltinCells().back() >= tables.builtinCells().size())
        encoder.fail(ProgramTableRule::TableOutOfBounds);
}

static StringID findIdentifierText(const TableEncoder& encoder, uint32_t number)
{
    return number < encoder.identifiers.count() ? encoder.identifiers.at(number).text : StringID();
}

// WTF::codePointCompare() orders by UTF-16 code unit.
static std::strong_ordering compareCodeUnits(const StringTable& strings, StringID first, StringID second)
{
    Vector<char16_t> firstBuffer;
    Vector<char16_t> secondBuffer;
    return codePointCompare(strings.viewOf(first, firstBuffer), strings.viewOf(second, secondBuffer));
}

template<typename Entry>
static void checkModuleNames(const TableEncoder& encoder, std::span<const Entry> list, uint32_t first, uint32_t end)
{
    if (first > end || end > list.size()) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    StringID previous;
    for (const Entry& entry : list.subspan(first, end - first)) {
        StringID name = findIdentifierText(encoder, entry.name);
        if (!name) {
            encoder.fail(ProgramTableRule::NameWithoutIdentifier);
            return;
        }
        if (previous && compareCodeUnits(encoder.program.strings, previous, name) >= 0) {
            encoder.fail(ProgramTableRule::TableNotSorted);
            return;
        }
        previous = name;
    }
}

static void checkModuleTables(const TableEncoder& encoder, const ProgramTables& tables)
{
    auto& strings = encoder.program.strings;
    auto modules = tables.modules();
    size_t numberOfModules = encoder.program.modules.size();
    if (modules.size() != numberOfModules + 1) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    for (size_t index = 0; index < numberOfModules; ++index) {
        checkModuleNames(encoder, tables.moduleExports(), modules[index].firstExport, modules[index + 1].firstExport);
        checkModuleNames(encoder, tables.syntheticExports(), modules[index].firstSyntheticExport, modules[index + 1].firstSyntheticExport);
    }

    bool everyPathExists = std::ranges::all_of(tables.modulesByPath(), [&](uint32_t module) {
        return module < numberOfModules && modules[module].publicPath < strings.size();
    });
    if (!everyPathExists) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    bool isSorted = isStrictlyAscending(tables.modulesByPath(), [&](uint32_t module) {
        return viewOf(strings[StringID(modules[module].publicPath)]);
    });
    if (!isSorted)
        encoder.fail(ProgramTableRule::TableNotSorted);
}

static void checkSortedTables(const TableEncoder& encoder, const ProgramTables& tables)
{
    auto checkAscending = [&](auto entries, const auto& keyOf) {
        if (!isStrictlyAscending(entries, keyOf))
            encoder.fail(ProgramTableRule::TableNotSorted);
    };
    checkAscending(tables.constructors(), &ConstructorData::function);
    checkAscending(tables.boundFunctions(), &BoundFunctionData::function);
    checkAscending(tables.constructEntrypoints(), &ConstructEntrypointData::function);
    checkAscending(tables.implementedBuiltins(), &ImplementedBuiltinData::function);
    checkAscending(tables.staticsRoots(), &StaticsRange::offset);
    checkAscending(tables.staticsRuns(), &StaticsRun::firstWord);
    checkAscending(tables.requireEdges(), [](const RequireEdgeData& edge) {
        return std::pair { edge.referrer, edge.target };
    });
    checkAscending(tables.moduleResetSlots(), [](const ModuleResetSlotData& reset) {
        return std::pair { reset.module, static_cast<uint32_t>(reset.slot.offset) };
    });
    checkAscending(tables.compiledRuntimeModules(), &CompiledRuntimeModuleData::registryID);
    checkAscending(tables.compiledRuntimeFunctions(), &CompiledRuntimeFunctionData::id);
    checkBuiltinCells(encoder, tables);
    checkConstantBuiltinMembers(encoder, tables);
    checkClosedAbsentNames(encoder, tables);
    checkModuleTables(encoder, tables);
}

static void checkListedFunctions(const TableEncoder& encoder, const ProgramTables& tables)
{
    const Program& program = encoder.program;
    for (size_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            continue;
        FunctionID id(index);
        FunctionID boundEntrypoint = function.role == FunctionRole::BoundFunction ? encoder.findEntrypoint(id) : FunctionID();
        if (boundEntrypoint && !std::ranges::binary_search(tables.boundFunctions(), boundEntrypoint.index(), { }, &BoundFunctionData::function))
            encoder.fail(ProgramTableRule::FunctionNotListed, { }, id);
        auto* entrypoint = function.creator.tryAs<CreatorRecord::Entrypoint>();
        bool hasConstructEntrypoint = function.role == FunctionRole::GenericEntrypoint && entrypoint && entrypoint->constructEntrypoint;
        if (hasConstructEntrypoint && !std::ranges::binary_search(tables.constructEntrypoints(), id.index(), { }, &ConstructEntrypointData::function))
            encoder.fail(ProgramTableRule::FunctionNotListed, { }, id);
    }
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        FunctionID constructor = program.layouts[index].constructor;
        FunctionID entrypoint = encoder.findEntrypoint(constructor);
        if (entrypoint &&!std::ranges::binary_search(tables.constructors(), entrypoint.index(), { }, &ConstructorData::function))
            encoder.fail(ProgramTableRule::FunctionNotListed, LayoutID(index), constructor);
    }
}

static void checkClassCaptures(const TableEncoder& encoder, const LayoutData& record, LayoutID id)
{
    if (record.classData().environment != LayoutData::noBase)
        return;
    auto checkMethods = [&](std::span<const MethodData> methods) {
        for (const MethodData& method : methods) {
            if ((method.flags & methodTakesEnvironment) && !(method.flags & methodIsCreatedAtDefinition))
                encoder.fail(ProgramTableRule::ClassCapturesMismatch, id, FunctionID(method.function));
        }
    };
    checkMethods(record.methods());
    checkMethods(record.staticMethods());
}

static const LayoutData* tryGetLayoutData(std::span<const uint8_t> bytes, uint32_t offset)
{
    const LayoutData* layout = tryGetRecord<LayoutData>(bytes, offset);
    if (!layout || offset < sizeof(ProgramTables) + layout->prefixSize())
        return nullptr;
    return offset + sizeof(LayoutData) + layout->trailingSize() <= bytes.size() ? layout : nullptr;
}

static void checkObjectRecord(const TableEncoder& encoder, std::span<const uint8_t> bytes, uint32_t offset, LayoutID id, uint32_t shape)
{
    const LayoutData* layout = tryGetLayoutData(bytes, offset);
    if (!layout) {
        encoder.fail(ProgramTableRule::TableOutOfBounds, id);
        return;
    }
    if (layout->shape != shape || layout->index != id.index())
        encoder.fail(ProgramTableRule::ShapeWithoutLayout, id);
    auto hasUnknownFlag = [](const FieldData& field) {
        return !!(field.flags & ~allFieldFlags);
    };
    if (std::ranges::any_of(layout->fields(), hasUnknownFlag) || std::ranges::any_of(layout->staticFields(), hasUnknownFlag))
        encoder.fail(ProgramTableRule::UnknownFieldFlag, id);
    if (!layout->isClass())
        return;
    bool guardsAreSorted = isStrictlyAscending(layout->memberGuards(), [](const MemberGuardData& guard) {
        return std::tuple { guard.side(), guard.kind(), static_cast<uint32_t>(guard.name) };
    });
    if (!guardsAreSorted)
        encoder.fail(ProgramTableRule::TableNotSorted, id);
    checkClassCaptures(encoder, *layout, id);
}

static void checkEnvironmentRecord(const TableEncoder& encoder, std::span<const uint8_t> bytes, uint32_t offset, LayoutID id, uint32_t shape)
{
    const EnvironmentData* environment = tryGetRecord<EnvironmentData>(bytes, offset);
    if (!environment || offset + sizeof(EnvironmentData) + static_cast<uint64_t>(environment->numberOfDescribedVariables) * sizeof(VariableData) > bytes.size()) {
        encoder.fail(ProgramTableRule::TableOutOfBounds, id);
        return;
    }
    if (environment->shape != shape || environment->index != id.index())
        encoder.fail(ProgramTableRule::ShapeWithoutLayout, id);
}

static bool isClassTableWellFormed(const ProgramTables& tables, size_t numberOfLayouts)
{
    auto words = tables.classes();
    size_t numberOfGroups = (numberOfLayouts + 31) / 32;
    if (tables.numberOfLayoutIndices != numberOfLayouts || words.size() < numberOfGroups * 2)
        return false;
    size_t numberOfClasses = 0;
    for (size_t group = 0; group < numberOfGroups; ++group) {
        if (words[group * 2 + 1] != numberOfClasses)
            return false;
        numberOfClasses += std::popcount(words[group * 2]);
    }
    return words.size() == numberOfGroups * 2 + numberOfClasses;
}

static void checkLayoutRecords(const TableEncoder& encoder, const ProgramTables& tables, std::span<const uint8_t> bytes)
{
    const ShapeTable& shapes = encoder.placement.shapes();
    size_t numberOfLayouts = encoder.program.layouts.size();
    auto offsets = tables.layouts();

    size_t endOfShapes = std::max<size_t>(offsets.size(), static_cast<size_t>(shapes.numberOfShapes()) + 1);
    for (uint32_t shape = 0; shape < endOfShapes; ++shape) {
        uint32_t offset = shape < offsets.size() ? offsets[shape] : 0;
        LayoutID id = shapes.isAssigned(shape) ? shapes.shape(shape).layout : LayoutID();
        if (!offset != !id)
            encoder.fail(ProgramTableRule::ShapeWithoutLayout, id);
        else if (id && shape < tables.firstEnvironmentShape)
            checkObjectRecord(encoder, bytes, offset, id, shape);
        else if (id)
            checkEnvironmentRecord(encoder, bytes, offset, id, shape);
    }

    if (!isClassTableWellFormed(tables, numberOfLayouts)) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        uint32_t offset = tables.offsetOfClass(index);
        uint32_t generalShape = shapes.generalShapeOf(id);
        bool isGeneralShapeRecord = generalShape && generalShape < offsets.size() && offsets[generalShape] == offset;
        if (offset && !isGeneralShapeRecord)
            checkObjectRecord(encoder, bytes, offset, id, generalShape);
    }
}

static void checkFieldTable(const TableEncoder& encoder, const ProgramTables& tables)
{
    const uint32_t highestObjectShape = tables.firstEnvironmentShape - 1;
    const uint32_t typeOfAnyValue = encoder.typeOffset(TypeTable::anything);
    const auto numberOfColumns = static_cast<uint32_t>(std::min<uint64_t>(encoder.identifiers.count(), FieldTableEntry::maxName + 1));
    if (tables.numberOfFieldTableRows != highestObjectShape || highestObjectShape > tables.fieldTable().size() || tables.fieldRecordIndices().size() != tables.fieldTable().size() - highestObjectShape) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    auto entries = tables.fieldTableEntries();
    bool everyRowIsInBounds = std::ranges::all_of(tables.fieldTable().first(highestObjectShape), [&](uint32_t row) {
        return row <= entries.size() && entries.size() - row >= numberOfColumns;
    });
    if (!everyRowIsInBounds) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }

    // An entry belongs to the rows that begin its name in front of it.
    Vector<uint32_t> numberOfEntriesOfRow(FillWith { }, entries.size(), 0);
    for (uint32_t index = 0; index < entries.size(); ++index) {
        FieldTableEntry entry { entries[index] };
        if (entry.isEmpty())
            continue;
        if (entry.name() > index) {
            encoder.fail(ProgramTableRule::FieldTableEntryWithoutField);
            continue;
        }
        ++numberOfEntriesOfRow[index - entry.name()];
    }

    BitVector rowsOfShapes(entries.size());
    for (uint32_t shape = 1; shape <= highestObjectShape; ++shape) {
        uint32_t row = tables.fieldTableRowOf(shape);
        rowsOfShapes.quickSet(row);
        uint32_t numberOfFieldsFound = 0;
        LayoutID id = encoder.placement.shapes().shape(shape).layout;
        if (tables.hasLayoutForShape(shape)) {
            const LayoutData& layout = tables.layoutForShape(shape);
            for (const FieldData& field : layout.fields()) {
                if (!FieldTableEntry::hasColumn(field.name))
                    continue;
                ++numberOfFieldsFound;
                uint32_t index = row + field.name;
                if (findFieldInTable(tables, shape, field.name) != layout.indexOf(field) || FieldTableEntry { entries[index] } != FieldTableEntry::forField(layout, field)
                    || tables.fieldRecordIndices()[index] != FieldRecordIndex::create(field, layout.indexOf(field), tables.typeAt(field.type), typeOfAnyValue).bits)
                    encoder.fail(ProgramTableRule::FieldMissingFromFieldTable, id);
            }
        }
        if (numberOfEntriesOfRow[row] != numberOfFieldsFound)
            encoder.fail(ProgramTableRule::FieldTableEntryWithoutField, id);
    }
    for (uint32_t row = 0; row < entries.size(); ++row) {
        if (numberOfEntriesOfRow[row] && !rowsOfShapes.quickGet(row))
            encoder.fail(ProgramTableRule::FieldTableEntryWithoutField);
    }
}

// In a plan, String says that some string passes. In a record it says that every string passes.
static uint16_t expectedPrimitivesOf(const TypeTestPlan& plan)
{
    if (plan.stringLiterals.isEmpty())
        return plan.passingPrimitives;
    return (plan.passingPrimitives & ~Primitive::String) | Primitive::LiteralString;
}

static ExpectedRuntimeType computeExpectedRuntimeType(const TableEncoder& encoder, TypeID id)
{
    const Program& program = encoder.program;
    TypeTestPlan plan = computeTypeTestPlanFromAnything(program, program.at(id).withoutProofs());
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        return { .id = id, .primitives = program.at(TypeTable::anything).primitives, .cellTests = { CellTest::isObject() } };
    case TypeTestVerdict::NeverPasses:
        return { .id = id };
    case TypeTestVerdict::NeedsTest:
        // A function without a generic entrypoint has no closures, so its test has no record.
        plan.cellTests.removeAllMatching([&](const CellTest& test) {
            return test.kind == CellTestKind::IsClosureOf && !encoder.findEntrypoint(test.function);
        });
        return { .id = id, .primitives = expectedPrimitivesOf(plan), .integers = plan.integers, .cellTests = WTF::move(plan.cellTests), .literalStrings = WTF::move(plan.stringLiterals) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const RuntimeType* tryGetRuntimeType(std::span<const uint8_t> bytes, uint32_t offset)
{
    const RuntimeType* type = tryGetRecord<RuntimeType>(bytes, offset);
    if (!type)
        return nullptr;
    return offset + RuntimeType::sizeInBytes(type->numberOfCellTests, type->numberOfLiteralStrings, type->flags) <= bytes.size() ? type : nullptr;
}

static bool isShapeTestOf(const TableEncoder& encoder, std::span<const uint8_t> bytes, uint32_t offset, LayoutSetID layouts)
{
    const ShapeTestData* encoded = tryGetRecord<ShapeTestData>(bytes, offset);
    if (!encoded)
        return false;
    const ShapeTable& shapes = encoder.placement.shapes();
    ShapeTestData expected = shapes.dataOf(shapes.testOf(layouts));
    return encoded->first == expected.first && encoded->count == expected.count && encoded->bitSetOffset == expected.bitSetOffset;
}

static bool isEncodingOf(const TableEncoder& encoder, std::span<const uint8_t> bytes, const RuntimeCellTest& encoded, const CellTest& test)
{
    if (encoded.kind != test.kind)
        return false;
    switch (test.kind) {
    case CellTestKind::IsObject:
    case CellTestKind::IsCallable:
        return true;
    case CellTestKind::CellTypeIs:
        return encoded.cellTypes().first == test.cellTypes.first && encoded.cellTypes().last == test.cellTypes.last;
    case CellTestKind::ShapeIn:
        return isShapeTestOf(encoder, bytes, encoded.shapeTestOffset(), test.layouts);
    case CellTestKind::InheritsClassInfo:
    case CellTestKind::IsPristine:
        return encoded.builtinClass() == test.builtinClass;
    case CellTestKind::IsBuiltinCell:
        return encoder.placement.builtinCell(test.builtin) == encoded.builtinCellNumber();
    case CellTestKind::IsClosureOf:
        return encoded.function() == encoder.findEntrypoint(test.function);
    case CellTestKind::IsSymbol:
        return encoded.symbolKeyNumber() == encoder.identifierNumber(test.symbol);
    case CellTestKind::SharingModeIs:
        return encoded.operand == static_cast<uint32_t>(test.sharingMode);
    case CellTestKind::IsNamespaceOf:
        return encoded.module() == test.module;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isEncodingOf(const TableEncoder& encoder, std::span<const uint8_t> bytes, const RuntimeType& encoded, const ExpectedRuntimeType& expected)
{
    if (encoded.primitives != expected.primitives)
        return false;
    auto integers = encoded.integerRange();
    if (integers.has_value() != expected.integers.has_value())
        return false;
    if (integers && (integers->first != expected.integers->minimum || integers->second != expected.integers->maximum))
        return false;
    if (!std::ranges::equal(encoded.literalStrings(), expected.literalStrings.span(), { }, { }, &StringID::index))
        return false;
    auto cellTests = encoded.cellTests();
    if (cellTests.size() != expected.cellTests.size())
        return false;
    for (size_t index = 0; index < cellTests.size(); ++index) {
        if (!isEncodingOf(encoder, bytes, cellTests[index], expected.cellTests[index]))
            return false;
    }
    return true;
}

static void checkRuntimeTypes(const TableEncoder& encoder, const ProgramTables& tables, std::span<const uint8_t> bytes)
{
    const Program& program = encoder.program;
    std::span<const uint32_t> words = tables.types();
    uint32_t numberOfGroupWords = ProgramTables::numberOfTypeGroupWords(program.types.size());
    if (words.size() < 1 + numberOfGroupWords || words[0] != program.types.size()) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    size_t numberOfRecords = 0;
    for (uint32_t group = 0; group < numberOfGroupWords; group += 2) {
        if (words[1 + group + 1] != numberOfRecords) {
            encoder.fail(ProgramTableRule::TableOutOfBounds);
            return;
        }
        numberOfRecords += std::popcount(words[1 + group]);
    }
    if (words.size() != 1 + numberOfGroupWords + numberOfRecords) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    auto expectedTypes = collectInParallel<ExpectedRuntimeType>(encoder.build.parallelLoops(), program.types.size(), [&](unsigned index, Vector<ExpectedRuntimeType>& collected) {
        if (tables.offsetOfType(index))
            collected.append(computeExpectedRuntimeType(encoder, TypeID(index)));
    });
    for (const ExpectedRuntimeType& expected : expectedTypes) {
        const RuntimeType* encoded = tryGetRuntimeType(bytes, tables.offsetOfType(expected.id.index()));
        if (!encoded)
            encoder.fail(ProgramTableRule::TableOutOfBounds);
        else if (!isEncodingOf(encoder, bytes, *encoded, expected))
            encoder.build.diagnostics().add(InternalError { .kind = InternalErrorKind::RuntimeTypeStricterThanPlan, .count = expected.id.index() });
    }
}

static void checkObfuscation(const TableEncoder& encoder, const ProgramTables& tables)
{
    if (!encoder.obfuscates())
        return;
    uint32_t emptyString = encoder.program.strings.emptyString().index();
    auto modules = tables.modules();
    bool hasFunctionName = std::ranges::any_of(tables.functionNames(), [&](uint32_t name) {
        return name != emptyString;
    });
    bool hasDisplayPath = std::ranges::any_of(modules.first(std::min<size_t>(modules.size(), encoder.program.modules.size())), [&](const ModuleData& module) {
        return module.displayPath != emptyString;
    });
    bool hasTypeText = std::ranges::any_of(tables.typeSites(), [](const TypeSiteData& site) {
        return site.text != TypeSiteData::noText;
    });
    if (hasFunctionName || hasDisplayPath || hasTypeText)
        encoder.fail(ProgramTableRule::NameLeftInObfuscatedBuild);
}

static void checkColumns(const TableEncoder& encoder)
{
    for (size_t index : encoder.slots.keysWithColumn()) {
        std::optional<uint32_t> number = encoder.identifiers.numberOf(PropertyKeyID(index));
        if (!number || RowDisplacementTable::hasColumn(*number))
            continue;
        encoder.fail(ProgramTableRule::KeyWithoutPromisedColumn);
        return;
    }
}

static std::optional<BitVector> tryFindNodeHeaders(const ProgramTables& tables)
{
    auto words = tables.data();
    BitVector headers(words.size());
    for (size_t position = 1; position < words.size();) {
        DataNodeHeader header = DataNodeHeader::decode(words[position]);
        size_t remainingWords = words.size() - position;
        if (header.count >= remainingWords || DataNodeHeader::numberOfWords(header.kind(), header.count) > remainingWords)
            return std::nullopt;
        headers.quickSet(position);
        position += DataNodeHeader::numberOfWords(header.kind(), header.count);
    }
    return headers;
}

static ProgramTableRule findBrokenRule(const ProgramTables& tables, DataNodeHeader header)
{
    switch (header.kind()) {
    case DataNodeHeader::Kind::Array:
        return ProgramTableRule::None;
    case DataNodeHeader::Kind::Object:
        return header.operand < tables.allocations().size() ? ProgramTableRule::None : ProgramTableRule::TableOutOfBounds;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool holdsDataWords(DataNodeHeader header)
{
    switch (header.kind()) {
    case DataNodeHeader::Kind::Array:
        return header.operand != ArrayWithDouble;
    case DataNodeHeader::Kind::Object:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ProgramTableRule findBrokenRule(const TableEncoder& encoder, const ProgramTables& tables, const BitVector& headers, uint64_t word)
{
    uint32_t operand = DataWord::operandOf(word);
    switch (DataWord::kindOf(word)) {
    case DataWord::Kind::Immediate:
        return ProgramTableRule::None;
    case DataWord::Kind::StringConstant:
        return operand < encoder.inputs.stringConstants.count() ? ProgramTableRule::None : ProgramTableRule::TableOutOfBounds;
    case DataWord::Kind::BigIntConstant:
        return operand < tables.bigIntConstants().size() ? ProgramTableRule::None : ProgramTableRule::TableOutOfBounds;
    case DataWord::Kind::Node:
        return headers.get(operand) ? ProgramTableRule::None : ProgramTableRule::DataNotEncoded;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ProgramTableRule findBrokenDataRule(const TableEncoder& encoder, const ProgramTables& tables)
{
    std::optional<BitVector> headers = tryFindNodeHeaders(tables);
    if (!headers)
        return ProgramTableRule::TableOutOfBounds;
    auto words = tables.data();
    for (size_t position : *headers) {
        DataNodeHeader header = DataNodeHeader::decode(words[position]);
        if (ProgramTableRule rule = findBrokenRule(tables, header); rule != ProgramTableRule::None)
            return rule;
        if (!holdsDataWords(header))
            continue;
        for (uint64_t word : words.subspan(position + 1, header.count)) {
            if (ProgramTableRule rule = findBrokenRule(encoder, tables, *headers, word); rule != ProgramTableRule::None)
                return rule;
        }
    }
    return ProgramTableRule::None;
}

static void checkData(const TableEncoder& encoder, const ProgramTables& tables)
{
    ProgramTableRule rule = findBrokenDataRule(encoder, tables);
    if (rule != ProgramTableRule::None)
        encoder.fail(rule);
}

void checkProgramTables(const TableEncoder& encoder, std::span<const uint8_t> bytes)
{
    if (!checkTableBounds(encoder, bytes))
        return;
    auto& tables = reinterpretCastSpanStartTo<ProgramTables>(bytes);
    checkSortedTables(encoder, tables);
    checkListedFunctions(encoder, tables);
    checkLayoutRecords(encoder, tables, bytes);
    checkFieldTable(encoder, tables);
    checkRuntimeTypes(encoder, tables, bytes);
    checkObfuscation(encoder, tables);
    checkColumns(encoder);
    checkData(encoder, tables);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
